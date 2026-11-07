#include "desk.h"
#include "machine.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include "core/batch.hpp"                // stage 2: directory/single-file runs
#include "core/config.hpp"                // kDefaultTileOverlap, derive_default_output
#include "core/distributed/connection_card.hpp"  // stage 2: host card
#include "core/distributed/coordinator.hpp"      // stage 2: pairing
#include "core/distributed/identity.hpp"  // identity + trust paths
#include "core/distributed/protocol.hpp"  // Serve frames + Protocol framing
#include "core/model_downloader.hpp"      // stage 2: release manifest + download
#include "core/model_families.hpp"        // S3: the one family/grouping table
#include "core/model_install.hpp"         // stage 2: staged import
#include "core/model_manager.hpp"         // stage 2: scan + default_models_dir
#include "core/model_metadata.hpp"        // scene/recommended classification
#include "core/serve/daemon.hpp"          // default_serve_socket_path (inline)
#include "core/serve/daemon_control.hpp"  // stage 2: supervision
#include "utils/encoder_options.hpp"
#include "utils/unix_socket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

constexpr int CoalesceMs = 70;
constexpr int PrefsSaveMs = 400;

#ifdef Q_OS_WIN
// Minimum upix-cli the desktop talks to. The GUI's progress display rides on
// UPIX_PROGRESS=json events, which landed after the released cli-v0.1.0
// (commit 6dddc1c) — bump together with the CLI contract, and keep xmake's
// set_version() at or above this so same-repo builds never warn about themselves.
constexpr const char* kMinCliVersion = "0.2.0";

// Leading "v", then the first digits-and-dots run: "v0.2.0-3-gabc" → 0.2.0.
// Unparseable (dev builds) counts as compatible; we warn, never block.
bool cliVersionAtLeast(const QString& version, const char* min) {
    const QRegularExpression leadingNums{QStringLiteral("^v?(\\d+(?:\\.\\d+)*)")};
    const auto m = leadingNums.match(version.trimmed());
    if (!m.hasMatch()) return true;
    const auto part = [](const QString& s, int i) {
        const QStringList bits = s.split(QLatin1Char('.'));
        return i < bits.size() ? bits[i].toInt() : 0;
    };
    const QString v = m.captured(1);
    const QStringList minBits = QString::fromLatin1(min).split(QLatin1Char('.'));
    const auto minPart = [&minBits](int i) {
        return i < minBits.size() ? minBits[i].toInt() : 0;
    };
    for (int i = 0; i < 3; ++i) {
        if (part(v, i) != minPart(i)) return part(v, i) > minPart(i);
    }
    return true;
}
#endif  // Q_OS_WIN

#ifndef UPIX_VERSION
#define UPIX_VERSION "dev"
#endif

QString prefsPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
        .filePath(QStringLiteral("upix/desktop.json"));
}

QStringList imageExtensions() {
    return {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("webp"), QStringLiteral("avif")};
}

bool isImageFile(const QString& path) {
    return imageExtensions().contains(QFileInfo(path).suffix().toLower());
}

QVariantMap inputEntry(const QString& path, bool dir) {
    return QVariantMap{
        {"path", path},
        {"url", QUrl::fromLocalFile(path).toString()},
        {"dir", dir},
    };
}

// Grouped catalog for ModelsPage (share/core/model_families.hpp is the single
// source of the table — Grouping.js used to carry a private copy). Shape is
// what G.groups() used to return: [{label, items: [{m, short}]}].
QVariantList groupedCatalog(const QVariantList& models) {
    std::vector<std::string> names;
    QHash<QString, QVariantMap> byName;
    names.reserve(models.size());
    for (const QVariant& v : models) {
        const QVariantMap m = v.toMap();
        const QString name = m.value(QStringLiteral("name")).toString();
        byName[name] = m;
        names.push_back(name.toStdString());
    }
    QVariantList out;
    for (const auto& g : upix::groups(names)) {
        QVariantList items;
        for (const auto& it : g.items) {
            items.append(QVariantMap{
                {"m", byName.value(QString::fromStdString(it.name))},
                {"short", QString::fromStdString(it.short_name)},
            });
        }
        out.append(QVariantMap{
            {"label", QString::fromStdString(g.label)},
            {"items", items},
        });
    }
    return out;
}

QVariantMap defaultSnap() {
    return QVariantMap{
        {"cliFound", false},
        {"cliVersion", QString()},
        {"cliCompatible", true},
        {"inputs", QVariantList{}},
        {"folderCount", 0},
        {"cover", QString()},
        {"model", QString()},
        {"format", QString()},
        {"quality", 90},
        {"gpu", true},
        {"autoTile", true},
        {"tileSize", 256},
        {"force", false},
        {"jobs", 4},
        {"installed", QVariantList{}},
        {"installedGroups", QVariantList{}},
        {"remote", QVariantList{}},
        {"remoteError", QString()},
        {"modelsDir", QString()},
        {"downloading", QString()},
        {"downloadPct", -1},
        {"job", QVariantMap{{"phase", QStringLiteral("idle")},
                            {"message", QString()},
                            {"index", 0},
                            {"total", 0},
                            {"percent", 0},
                            {"tilesDone", 0},
                            {"tilesTotal", 0},
                            {"etaSec", 0},
                            {"current", QString()},
                            {"outputs", QVariantList{}}}},
        {"host", QVariantMap{{"running", false},
                             {"qr", QString()},
                             {"fingerprint", QString()},
                             {"ips", QVariantList{}},
                             {"port", 9527},
                             {"taskPercent", -1},
                             {"log", QVariantList{}},
                             {"pendingDevice", QString()},
                             {"pendingFp", QString()}}},
        {"daemon", QVariantMap{{"running", false}, {"label", QStringLiteral("…")},
                               {"pid", 0}, {"socketName", QString()},
                               {"socketMissing", false}}},
        {"notice", QString()},
        {"noticeSeq", 0},
    };
}

// Fixture snapshots for --shot (UPIX_FAKE). States match the web-era fixtures.
QVariantMap fakeSnap(const QString& state) {
    QVariantMap s = defaultSnap();
    s["cliFound"] = true;
    s["cliVersion"] = QStringLiteral("v0.2.0 avif:system-dlopen");
    s["model"] = QStringLiteral("AnimeSharp-x4");
    s["modelsDir"] = QStringLiteral("/home/xihale/.local/share/upix/models");
    const QVariantList installedModels = QVariantList{
        QVariantMap{{"name", QStringLiteral("Real-ESRGANv3-anime-x2")}, {"scale", 2}, {"size", QStringLiteral("1 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", true}},
        QVariantMap{{"name", QStringLiteral("AnimeSharp-x4")}, {"scale", 4}, {"size", QStringLiteral("31 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", true}},
        QVariantMap{{"name", QStringLiteral("Real-CUGAN-denoise-x2")}, {"scale", 2}, {"size", QStringLiteral("2 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", false}},
        QVariantMap{{"name", QStringLiteral("Remacri-x4")}, {"scale", 4}, {"size", QStringLiteral("31 MB")}, {"scene", QStringLiteral("综合")}, {"recommended", true}},
    };
    s["installed"] = installedModels;
    s["installedGroups"] = groupedCatalog(installedModels);
    s["remote"] = QVariantList{
        QVariantMap{{"name", QStringLiteral("BSRGAN-x4")}, {"scale", 4}, {"size", QStringLiteral("29.5 MB")}, {"scene", QStringLiteral("照片")}, {"recommended", false}},
        QVariantMap{{"name", QStringLiteral("UltraSharp-x4")}, {"scale", 4}, {"size", QStringLiteral("31 MB")}, {"scene", QStringLiteral("综合")}, {"recommended", true}},
        QVariantMap{{"name", QStringLiteral("DeJPG-RealPLKSR-x1")}, {"scale", 1}, {"size", QStringLiteral("13.0 MB")}, {"scene", QStringLiteral("照片·修复")}, {"recommended", false}},
    };
    s["daemon"] = QVariantMap{{"running", true},
                              {"label", QStringLiteral("running (pid 4321, socket /home/u/.cache/upix/serve.sock)")},
                              {"pid", 4321},
                              {"socketName", QStringLiteral("serve.sock")},
                              {"socketMissing", false}};
    s["host"] = QVariantMap{{"running", false},
                            {"qr", QString()},
                            {"fingerprint", QStringLiteral("EC61F2B66D4C25C4")},
                            {"ips", QVariantList{QStringLiteral("192.168.0.126"), QStringLiteral("10.210.210.2")}},
                            {"port", 9527},
                            {"taskPercent", -1},
                            {"log", QVariantList{}},
                            {"pendingDevice", QString()},
                            {"pendingFp", QString()}};

    const QString scene =
        QUrl::fromLocalFile(QStringLiteral(FIXTURE_DIR) + QStringLiteral("/scene.svg"))
            .toString();
    const QString sceneAfter =
        QUrl::fromLocalFile(QStringLiteral(FIXTURE_DIR) +
                            QStringLiteral("/scene-after.svg"))
            .toString();

    if (state == QStringLiteral("empty")) return s;
    if (state == QStringLiteral("host-idle")) return s;
    if (state == QStringLiteral("format")) {
        // Segmented indicator proof: current must follow snap.format.
        s["format"] = QStringLiteral("jpg");
        return s;
    }
    if (state == QStringLiteral("host-blank")) {
        // First-run idle: nothing paired yet, so fp/addresses are empty.
        QVariantMap h = s["host"].toMap();
        h["fingerprint"] = QString();
        h["ips"] = QVariantList{};
        s["host"] = h;
        return s;
    }
    if (state == QStringLiteral("picked")) {
        s["inputs"] = QVariantList{inputEntry(QStringLiteral(FIXTURE_DIR) + QStringLiteral("/scene.svg"), false)};
        return s;
    }
    if (state == QStringLiteral("running")) {
        s["inputs"] = QVariantList{inputEntry(QStringLiteral(FIXTURE_DIR) + QStringLiteral("/scene.svg"), false)};
        s["job"] = QVariantMap{{"phase", QStringLiteral("running")},
                               {"message", QString()},
                               {"index", 1},
                               {"total", 1},
                               {"percent", 42},
                               {"tilesDone", 5},
                               {"tilesTotal", 12},
                               {"etaSec", 3},
                               {"current", QStringLiteral("窗台绣球.jpg")},
                               {"outputs", QVariantList{}}};
        return s;
    }
    if (state == QStringLiteral("done")) {
        s["inputs"] = QVariantList{inputEntry(QStringLiteral(FIXTURE_DIR) + QStringLiteral("/scene.svg"), false)};
        s["job"] = QVariantMap{{"phase", QStringLiteral("done")},
                               {"message", QStringLiteral("完成 · 2560x1680")},
                               {"index", 1},
                               {"total", 1},
                               {"percent", 100},
                               {"tilesDone", 12},
                               {"tilesTotal", 12},
                               {"etaSec", 0},
                               {"current", QString()},
                               {"outputs", QVariantList{sceneAfter}}};
        return s;
    }
    if (state == QStringLiteral("error")) {
        s["inputs"] = QVariantList{inputEntry(QStringLiteral(FIXTURE_DIR) + QStringLiteral("/scene.svg"), false)};
        s["job"] = QVariantMap{{"phase", QStringLiteral("error")},
                               {"message", QStringLiteral("模型加载失败: weight 不匹配")},
                               {"index", 0}, {"total", 1}, {"percent", 0},
                               {"tilesDone", 0}, {"tilesTotal", 0}, {"etaSec", 0},
                               {"current", QString()}, {"outputs", QVariantList{}}};
        return s;
    }
    if (state == QStringLiteral("models")) {
        s["downloading"] = QStringLiteral("BSRGAN-x4");
        s["downloadPct"] = 60;
        return s;
    }
    if (state == QStringLiteral("host") || state == QStringLiteral("pairing")) {
        QVariantMap h = s["host"].toMap();
        h["running"] = true;
        h["qr"] = QStringLiteral(
            "{\"v\":2,\"ip\":\"192.168.0.126\",\"port\":9527,"
            "\"fp\":\"EC61F2B66D4C25C4\",\"pk\":\"dGVzdCBwdWJsaWMga2V5IDMyIGJ5dGVzISE=\"}");
        h["taskPercent"] = 66;
        h["log"] = QVariantList{QStringLiteral("Coordinator running."),
                                QStringLiteral("手机已连接: Pixel 9 Pro")};
        if (state == QStringLiteral("pairing")) {
            h["pendingDevice"] = QStringLiteral("Pixel 9 Pro");
            h["pendingFp"] = QStringLiteral("AB12CD34EF567788");
        }
        s["host"] = h;
    }
    return s;
}

} // namespace

// One in-process delegation to the warm engine daemon. The worker thread owns
// the blocking socket I/O; events are marshalled onto the GUI thread as the
// same machine-protocol JSON lines the CLI job stream speaks, so handleJobLine
// and finishJob stay the only consumers. Desk deletes the job (joining the
// thread) on completion, cancellation, or shutdown.
struct Desk::ServeJob {
    explicit ServeJob(Desk* d) : desk(d) {}
    ~ServeJob() {
        cancelled.store(true, std::memory_order_relaxed);
        socket.shutdown_wake();
        if (thread.joinable()) thread.join();
    }
    void requestCancel() {
        cancelled.store(true, std::memory_order_relaxed);
        // close() from a foreign thread does not reliably wake a blocked
        // recv; shutdown() does.
        socket.shutdown_wake();
    }
    QPointer<Desk> desk;
    upix::UnixSocket socket;
    std::thread thread;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    QString inPath;
    QString model;
    QString format;
    int quality = 90;
    int tileSize = 256;
    bool force = false;
};

Desk::~Desk() {
#ifdef Q_OS_UNIX
    // Joins the delegation workers (their queued callbacks target this
    // object, so nothing may outlive the member state they touch).
    delete m_serveJob;
    m_serveJob = nullptr;
    delete m_batchJob;
    m_batchJob = nullptr;
    delete m_hostRun;
    m_hostRun = nullptr;
#endif
    if (g_desk == this) g_desk = nullptr;
}

#ifdef Q_OS_UNIX

// Stage-2 batch runs (docs/SPEC-frontends.md §3): share's run_batch on a
// worker thread — directories, CPU, auto-tile, and any single file the warm
// daemon cannot serve. Events are marshalled onto the GUI thread as the same
// machine JSON lines the CLI job stream speaks; Desk deletes the job
// (joining the thread) on completion, cancellation, or shutdown.
struct Desk::BatchJob {
    explicit BatchJob(Desk* d) : desk(d) {}
    ~BatchJob() {
        cancelled.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }
    void requestCancel() { cancelled.store(true, std::memory_order_relaxed); }

    QPointer<Desk> desk;
    std::thread thread;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    upix::BatchRequest request;
};

// Stage-2 host: the pairing coordinator lives on a worker thread of THIS
// process. Card, approvals and progress are marshalled as snapshot edits;
// approvals resolve through resolve_approval (the CLI wrote "y\n" to a
// subprocess stdin — that boundary is gone).
struct Desk::HostRun {
    explicit HostRun(Desk* d) : desk(d) {}
    ~HostRun() {
        stopping.store(true, std::memory_order_relaxed);
        coordinator.stop();
        if (thread.joinable()) thread.join();
    }
    void requestStop() {
        stopping.store(true, std::memory_order_relaxed);
        coordinator.stop();
    }
    void decide(bool allow) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        if (pending) coordinator.resolve_approval(pending->id, allow);
        pending.reset();
    }

    QPointer<Desk> desk;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<int> taskPct{-1};
    std::mutex pendingMutex;
    std::optional<upix::PendingApproval> pending;
    upix::Coordinator coordinator;
    uint16_t port = 0;
    bool stoppingIsUserAction = false;  // read by the GUI thread after join
};

#endif  // Q_OS_UNIX

Desk::Desk(QObject* parent) : QObject(parent) {
    g_desk = this;
    m_snap = defaultSnap();
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(CoalesceMs);
    connect(&m_coalesce, &QTimer::timeout, this, &Desk::notifyNow);
    m_prefsSave.setSingleShot(true);
    m_prefsSave.setInterval(PrefsSaveMs);
    connect(&m_prefsSave, &QTimer::timeout, this, &Desk::flushPrefs);
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                &Desk::flushPrefs);

    loadPrefs();
    // Fixture mode (--shot) never touches the CLI.
    if (!qEnvironmentVariableIsEmpty("UPIX_FAKE")) return;
    boot();
}

#ifdef Q_OS_WIN
QString Desk::findCli() const {
    // 1. explicit override
    const QString env = qEnvironmentVariable("UPIX_CLI");
    if (!env.isEmpty() && QFileInfo(env).isExecutable() && QFileInfo(env).isFile())
        return env;

    // 2. sibling of the exe, exact name only. The GUI binary itself must
    //    NEVER match — this guard is the fix for the self-spawn freeze.
    const QDir exeDir = QFileInfo(QCoreApplication::applicationFilePath()).absoluteDir();
    for (const QString& name : {QStringLiteral("upix-cli"), QStringLiteral("upix-cli.exe")}) {
        const QString p = exeDir.filePath(name);
        if (QFileInfo(p).isFile() && QFileInfo(p).isExecutable()) return p;
    }

    // 3. PATH: upix-cli first, then a plain `upix` that is NOT this binary.
    for (const QString& name :
         {QStringLiteral("upix-cli"), QStringLiteral("upix"), QStringLiteral("upix-cli.exe")}) {
        const QString p = QStandardPaths::findExecutable(name);
        if (p.isEmpty()) continue;
        QFileInfo info(p);
        if (info.isFile() && info.isExecutable() &&
            info.canonicalFilePath() !=
                QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath())
            return p;
    }
    return QString();
}
#endif  // Q_OS_WIN

void Desk::boot() {
#ifdef Q_OS_UNIX
    // Stage 2 (docs/SPEC-frontends.md §3): the full core is linked in — no
    // CLI probe, no version gate. cliFound now means "backend ready", which
    // is always true here; QML gates read it unchanged.
    s()["cliFound"] = true;
    s()["cliVersion"] = QStringLiteral(UPIX_VERSION);
    s()["cliCompatible"] = true;
    touch();
    refreshModels();
    refreshDaemonStatus();
#else
    const QString cli = findCli();
    s()["cliFound"] = !cli.isEmpty();
    touch();

    if (cli.isEmpty()) {
        setNotice(QString());
        return;
    }
    // version probe (cheap) then models + daemon in background
    if (QProcess* p = spawn(QStringLiteral("version"), {QStringLiteral("--version")})) {
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
            const QString out = QString::fromUtf8(p->readAll());
            if (code == 0 && !out.trimmed().isEmpty()) {
                // The version line is machine-readable by definition.
                const QString version = out.section(QLatin1Char('\n'), 0, 0).trimmed();
                s()["cliVersion"] = version;
                s()["cliCompatible"] = cliVersionAtLeast(version, kMinCliVersion);
                touch();
            }
        });
    }
    refreshModels();
    refreshDaemonStatus();
#endif
}

QProcess* Desk::spawn(const QString& key, const QStringList& args,
                      const QStringList& env) {
#ifdef Q_OS_WIN
    const QString cli = findCli();
    if (cli.isEmpty()) return nullptr;

    // Take (never hold a reference into) the slot: an exec failure is reported
    // from inside waitForStarted() below, which can run finishJob() and remove
    // the entry — a `QProcess*&` here would then be dereferencing freed hash
    // storage.
    if (QProcess* stale = m_procs.take(key)) {
        stale->disconnect(this);
        stale->deleteLater();
    }
    QProcess* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    // The CLI is a console-subsystem binary; without CREATE_NO_WINDOW every
    // spawn (version probe, models, daemon status, every job) flashes a
    // console window. Literal over <windows.h> to keep this TU lean.
    proc->setCreateProcessArgumentsArgument([](QProcess::CreateProcessArguments* a) {
        a->flags |= 0x08000000;  // CREATE_NO_WINDOW
    });
#endif
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    for (const QString& kv : env) {
        const int eq = kv.indexOf(QLatin1Char('='));
        e.insert(kv.left(eq), kv.mid(eq + 1));
    }
    proc->setProcessEnvironment(e);
    connect(proc, &QProcess::errorOccurred, this, [this, key, proc](QProcess::ProcessError) {
        // Only the job stream is stateful; a superseded process must not touch
        // the run it no longer belongs to.
        if (key != QLatin1String("job") || m_procs.value(key) != proc) return;
        readJobOutput(proc, true);  // keep whatever it printed before dying
        finishJob(-1);
    });
    m_procs.insert(key, proc);
    proc->start(cli, args);
    if (!proc->waitForStarted(3000)) {
        if (m_procs.value(key) == proc) m_procs.remove(key);
        proc->disconnect(this);
        proc->deleteLater();
        return nullptr;
    }
    return proc;
#else
    // UNIX uses in-process workers end to end (stage 2); the CLI bridge is
    // Windows-only. No smoke path reaches this either (runSmoke resolves the
    // CLI itself).
    Q_UNUSED(key)
    Q_UNUSED(args)
    Q_UNUSED(env)
    return nullptr;
#endif
}

void Desk::touch() {
    if (!m_coalesce.isActive()) m_coalesce.start();
}

void Desk::notifyNow() {
    emit snapChanged();
}

void Desk::setNotice(const QString& msg) {
    if (s()["notice"] == msg && msg.isEmpty()) return;
    s()["notice"] = msg;
    s()["noticeSeq"] = s()["noticeSeq"].toInt() + 1;
    touch();
}

bool Desk::jobRunning() const {
    return m_snap.value(QStringLiteral("job"))
               .toMap()
               .value(QStringLiteral("phase"))
               .toString() == QLatin1String("running");
}

// Drop a stale result card (done/error/cancelled) when the selection changes.
// A live run owns the card: resetting it mid-run made the queue unobservable
// and stranded every input that had not been started yet.
void Desk::resetJobCard() {
    if (jobRunning()) return;
    s()["job"] = QVariantMap(defaultSnap().value("job").toMap());
}

void Desk::loadPrefs() {
    QFile f(prefsPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const char* keys[] = {"model", "format", "quality", "gpu",
                          "autoTile", "tileSize", "force", "jobs"};
    for (const char* k : keys) {
        if (o.contains(QLatin1String(k))) s()[QLatin1String(k)] = o.value(QLatin1String(k)).toVariant();
    }
    if (o.contains("port")) {
        QVariantMap h = s()["host"].toMap();
        h.insert(QStringLiteral("port"), o.value(QStringLiteral("port")).toInt());
        s()["host"] = h;
    }
}

// Dragging 质量/分块 emitted setQuality()/setTileSize() per pointer move, each
// one rewriting this file. Coalesce the write; flushPrefs() (aboutToQuit) makes
// sure the last edit of a session is not lost.
void Desk::savePrefs() { m_prefsSave.start(); }

void Desk::flushPrefs() {
    if (!m_prefsSave.isActive()) return;  // nothing pending: never write on exit
    m_prefsSave.stop();
    QJsonObject o;
    o.insert(QStringLiteral("model"), m_snap.value(QStringLiteral("model")).toString());
    o.insert(QStringLiteral("format"), m_snap.value(QStringLiteral("format")).toString());
    o.insert(QStringLiteral("quality"), m_snap.value(QStringLiteral("quality")).toInt());
    o.insert(QStringLiteral("gpu"), m_snap.value(QStringLiteral("gpu")).toBool());
    o.insert(QStringLiteral("autoTile"), m_snap.value(QStringLiteral("autoTile")).toBool());
    o.insert(QStringLiteral("tileSize"), m_snap.value(QStringLiteral("tileSize")).toInt());
    o.insert(QStringLiteral("force"), m_snap.value(QStringLiteral("force")).toBool());
    o.insert(QStringLiteral("jobs"), m_snap.value(QStringLiteral("jobs")).toInt());
    o.insert(QStringLiteral("port"),
             m_snap.value(QStringLiteral("host")).toMap().value(QStringLiteral("port")).toInt());
    QDir().mkpath(QFileInfo(prefsPath()).absolutePath());
    QFile f(prefsPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// ---- inputs ------------------------------------------------------------

void Desk::addPaths(const QStringList& paths) {
    QVariantList inputs = s()["inputs"].toList();
    int folderCount = s()["folderCount"].toInt();
    QString cover = s()["cover"].toString();
    int skipped = 0;

    for (const QString& raw : paths) {
        QString path = raw;
        if (path.startsWith(QStringLiteral("file://")))
            path = QUrl(path).toLocalFile();
        path = QDir::cleanPath(path);
        QFileInfo info(path);
        if (!info.exists()) {
            ++skipped;
            continue;
        }

        const bool isDir = info.isDir();
        if (!isDir && !isImageFile(path)) {
            ++skipped;
            continue;
        }
        bool duplicate = false;
        for (const QVariant& value : inputs) {
            const QVariantMap entry = value.toMap();
            if (entry.value("path").toString() == path && entry.value("dir").toBool() == isDir) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        // `count`/`cover` are kept per entry so removing one input can restore
        // the badge count and the stage cover (they used to go stale).
        QVariantMap entry = inputEntry(path, isDir);
        if (isDir) {
            QStringList filters = imageExtensions();
            for (QString& extension : filters) extension.prepend(QStringLiteral("*."));
            QDirIterator it(path, filters, QDir::Files, QDirIterator::Subdirectories);
            QStringList files;
            while (it.hasNext()) files.append(it.next());
            files.sort();
            if (files.isEmpty()) {
                setNotice(QStringLiteral("文件夹里没有图片"));
                continue;
            }
            folderCount += files.size();
            entry["count"] = files.size();
            entry["cover"] = QUrl::fromLocalFile(files.first()).toString();
        } else {
            folderCount += 1;
            entry["count"] = 1;
            entry["cover"] = QUrl::fromLocalFile(path).toString();
        }
        if (cover.isEmpty()) cover = entry.value("cover").toString();
        inputs.append(entry);
    }
    s()["inputs"] = inputs;
    s()["folderCount"] = folderCount;
    s()["cover"] = cover;
    resetJobCard();
    touch();
    // A drop/pick that produced nothing is otherwise indistinguishable from a
    // drop that was ignored: say how many entries were dropped.
    if (skipped > 0)
        setNotice(QStringLiteral("已忽略 %1 个不支持的文件").arg(skipped));
}

void Desk::clearInputs() {
    s()["inputs"] = QVariantList{};
    s()["folderCount"] = 0;
    s()["cover"] = QString();
    resetJobCard();
    touch();
}

void Desk::removeInput(const QString& path) {
    QVariantList inputs;
    int folderCount = 0;
    QString cover;
    for (const QVariant& v : s()["inputs"].toList()) {
        const QVariantMap entry = v.toMap();
        if (entry.value("path") == path) continue;
        inputs.append(v);
        folderCount += entry.value("count").toInt();
        if (cover.isEmpty()) cover = entry.value("cover").toString();
    }
    s()["inputs"] = inputs;
    s()["folderCount"] = folderCount;
    s()["cover"] = cover;
    // Unchanged from before: only a pristine (idle) card is reset here; a
    // finished run keeps its outputs reachable from the remaining list.
    if (s()["job"].toMap().value("phase").toString() == QLatin1String("idle"))
        s()["job"] = QVariantMap(defaultSnap().value("job").toMap());
    touch();
}

// ---- settings ----------------------------------------------------------

void Desk::setModel(const QString& v) { s()["model"] = v; savePrefs(); touch(); }
void Desk::setFormat(const QString& v) { s()["format"] = v; savePrefs(); touch(); }
void Desk::setQuality(int v) { s()["quality"] = v; savePrefs(); touch(); }
void Desk::setGpu(bool v) { s()["gpu"] = v; savePrefs(); touch(); }
void Desk::setJobs(int v) { s()["jobs"] = v; savePrefs(); touch(); }
void Desk::setAutoTile(bool v) { s()["autoTile"] = v; savePrefs(); touch(); }
void Desk::setTileSize(int v) { s()["tileSize"] = v; savePrefs(); touch(); }
void Desk::setForce(bool v) { s()["force"] = v; savePrefs(); touch(); }

// ---- job ---------------------------------------------------------------

void Desk::start() {
    if (!s()["cliFound"].toBool()) {
        setNotice(QStringLiteral("未找到 upix-cli"));
        return;
    }
    // Ctrl+Return / a second CTA tap during a run used to restart the queue:
    // spawn() killed the in-flight CLI, cleared the collected outputs and
    // silently dropped every input that had not run yet.
    if (jobRunning()) {
        setNotice(QStringLiteral("正在处理中，先停止再开始"));
        return;
    }
    const QVariantList inputs = s()["inputs"].toList();
    if (inputs.isEmpty() || s()["model"].toString().isEmpty()) return;

    m_runSnap = s();
    m_runIndex = 0;
    m_cancelRequested = false;
    m_jobPending.clear();
    m_jobOutput.clear();
    QVariantMap job = defaultSnap().value("job").toMap();
    job["phase"] = QStringLiteral("running");
    job["total"] = inputs.size();
    job["index"] = 1;
    s()["job"] = job;
    touch();
    startNextJob();
}

void Desk::startNextJob() {
    const QVariantList inputs = m_runSnap.value("inputs").toList();
    if (m_cancelRequested || m_runIndex >= inputs.size()) {
        QVariantMap job = s()["job"].toMap();
        job["phase"] = m_cancelRequested ? QStringLiteral("cancelled") : QStringLiteral("done");
        if (!m_cancelRequested) job["percent"] = 100;
        if (job.value("message").toString().isEmpty())
            job["message"] = m_cancelRequested ? QStringLiteral("已取消") : QStringLiteral("完成");
        s()["job"] = job;
        touch();
        refreshDaemonStatus();
        return;
    }

    const QVariantMap input = inputs.at(m_runIndex).toMap();
    const QVariantMap settings = m_runSnap;
    const bool isDir = input.value("dir").toBool();
    const bool autoTile = settings.value("autoTile").toBool();

    QVariantMap job = s()["job"].toMap();
    job["index"] = m_runIndex + 1;
    job["percent"] = 0;
    job["tilesDone"] = 0;
    job["tilesTotal"] = 0;
    job["etaSec"] = 0;
    job["current"] = QFileInfo(input.value("path").toString()).fileName();
    s()["job"] = job;
    m_jobPending.clear();
    m_jobOutput.clear();

#ifdef Q_OS_UNIX
    // Warm-daemon delegation (docs/SPEC-frontends.md §3 stage 1): a single
    // file with a pinned tile size on the GPU rides the serve protocol
    // in-process. Everything else — dirs, auto-tile (the optimizer is not in
    // ServeRequest), CPU — runs the share batch core in-process (stage 2);
    // a daemon that will not answer falls back to the same core with its own
    // engine, so the CLI bridge is Windows-only now.
    if (!isDir && settings.value("gpu").toBool() && !autoTile) {
        startServeJob(input, settings);
        return;
    }
    startBatchJob();
#else
    spawnCliJob();
#endif
}

void Desk::spawnCliJob() {
    const QVariantList inputs = m_runSnap.value("inputs").toList();
    const QVariantMap input = inputs.at(m_runIndex).toMap();
    const QVariantMap settings = m_runSnap;
    QStringList args{QStringLiteral("-i"), input.value("path").toString(),
                     QStringLiteral("-m"), settings.value("model").toString(),
                     QStringLiteral("--output-quality"), QString::number(settings.value("quality").toInt())};
    const QString fmt = settings.value("format").toString();
    if (!fmt.isEmpty()) args << QStringLiteral("--output-format") << fmt;
    const bool isDir = input.value("dir").toBool();
    const bool autoTile = settings.value("autoTile").toBool();
    if (isDir && autoTile) args << QStringLiteral("--auto-tile");
    else if (!autoTile) args << QStringLiteral("--tile-size") << QString::number(settings.value("tileSize").toInt());
    if (settings.value("force").toBool()) args << QStringLiteral("-f");
    if (isDir) {
        if (settings.value("gpu").toBool()) args << QStringLiteral("--gpu");
        else args << QStringLiteral("-j") << QString::number(settings.value("jobs").toInt());
    }

    QProcess* process = spawn(QStringLiteral("job"), args, {QStringLiteral("UPIX_PROGRESS=json")});
    if (!process) {
        finishJob(-1);
        return;
    }
    connect(process, &QProcess::readyRead, this, [this, process] { readJobOutput(process); });
    connect(process, &QProcess::finished, this,
            [this, process](int code, QProcess::ExitStatus status) {
                readJobOutput(process, true);
                finishJob(code == 0 && status == QProcess::NormalExit ? 0 : 1);
            });
}

#ifdef Q_OS_UNIX


void Desk::startServeJob(const QVariantMap& input, const QVariantMap& settings) {
    m_serveJob = new ServeJob(this);
    ServeJob* job = m_serveJob;
    job->inPath = input.value(QStringLiteral("path")).toString();
    job->model = settings.value(QStringLiteral("model")).toString();
    job->format = settings.value(QStringLiteral("format")).toString();
    job->quality = settings.value(QStringLiteral("quality")).toInt();
    job->tileSize = settings.value(QStringLiteral("tileSize")).toInt();
    job->force = settings.value(QStringLiteral("force")).toBool();

    job->thread = std::thread([this, job] {
        // Marshalled into the GUI thread; `guard` makes a stalled delivery
        // harmless if Desk is already gone.
        auto emitLine = [this, job](const QString& line) {
            if (!job->desk) return;
            QMetaObject::invokeMethod(job->desk, [this, line] { handleJobLine(line); },
                                      Qt::QueuedConnection);
        };
        // 0 = ok, 1 = error (detail lands in m_jobOutput), 2 = daemon
        // unavailable → TRANSITIONAL fallback to the CLI path.
        auto finish = [this, job](int code, const QString& detail) {
            if (job->done.exchange(true)) return;
            if (!job->desk) return;
            QMetaObject::invokeMethod(job->desk, [this, code, detail] { onServeDone(code, detail); },
                                      Qt::QueuedConnection);
        };

        namespace fs = std::filesystem;
        // Test override (tst_desk_jobs spins a fake daemon on a tmp socket);
        // production resolves the same path the CLI daemon listens on.
        const QString override_ = qEnvironmentVariable("UPIX_SERVE_SOCKET");
        const std::string sock_path = !override_.isEmpty()
            ? override_.toStdString()
            : upix::default_serve_socket_path();
        if (sock_path.empty()) { finish(2, {}); return; }
        std::error_code ec;
        if (!fs::exists(sock_path, ec) || !job->socket.connect(sock_path).has_value()) {
            finish(2, {});
            return;
        }

        upix::ServeRequest req{};
        const std::string model = job->model.toStdString();
        std::memcpy(req.model_name, model.data(),
                    std::min(model.size(), sizeof(req.model_name) - 1));
        const std::string fmt = job->format.toStdString();
        std::memcpy(req.output_format, fmt.data(),
                    std::min(fmt.size(), sizeof(req.output_format) - 1));
        req.tile_size = job->tileSize;
        req.tile_overlap = upix::kDefaultTileOverlap;
        req.output_quality = job->quality;
        req.encoder = upix::to_wire(upix::EncoderOptions{});
        req.force = job->force ? 1 : 0;
        const std::string in_abs = fs::absolute(fs::path(job->inPath.toStdString()))
                                       .lexically_normal().string();
        req.input_path_length = static_cast<uint32_t>(in_abs.size());
        req.output_path_length = 0;  // daemon derives — same grammar as the CLI default
        std::vector<uint8_t> payload(sizeof(req) + req.input_path_length);
        std::memcpy(payload.data(), &req, sizeof(req));
        std::memcpy(payload.data() + sizeof(req), in_abs.data(), req.input_path_length);

        upix::MessageHeader h{};
        h.magic = upix::Protocol::MAGIC;
        h.type = upix::MessageType::MSG_SERVE_REQUEST;
        h.payload_length = static_cast<uint32_t>(payload.size());
        const auto frame = upix::Protocol::serialize(h, payload.data());
        if (!job->socket.send_all(frame.data(), frame.size()).has_value()) {
            finish(1, {});
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        std::vector<uint8_t> rhdr(upix::Protocol::HEADER_SIZE);
        while (true) {
            if (!job->socket.receive_all(rhdr.data(), rhdr.size()).has_value()) {
                finish(1, {});
                return;
            }
            const auto parsed = upix::Protocol::parse_header(rhdr);
            if (!parsed) { finish(1, {}); return; }
            if (parsed->type == upix::MessageType::MSG_SERVE_PROGRESS &&
                parsed->payload_length == sizeof(upix::ServeProgress)) {
                std::vector<uint8_t> pb(parsed->payload_length);
                if (!job->socket.receive_all(pb.data(), pb.size()).has_value() ||
                    !upix::Protocol::verify_frame(rhdr.data(), pb.data(), pb.size())) {
                    finish(1, {});
                    return;
                }
                upix::ServeProgress sp{};
                std::memcpy(&sp, pb.data(), sizeof(sp));
                if (sp.stage == 0 && sp.total_tiles > 0) {
                    const int done = static_cast<int>(sp.completed_tiles);
                    const int total = static_cast<int>(sp.total_tiles);
                    const int pct = std::min(100, done * 100 / total);
                    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started).count();
                    const int eta = done > 0
                        ? static_cast<int>(secs * (total - done) / done) : 0;
                    emitLine(QStringLiteral(
                                 R"({"t":"progress","pct":%1,"done":%2,"total":%3,"eta":%4})")
                                 .arg(pct).arg(done).arg(total).arg(eta));
                }
                continue;
            }
            if (parsed->type != upix::MessageType::MSG_SERVE_RESPONSE ||
                parsed->payload_length < sizeof(upix::ServeResponse) - 8) {
                finish(1, {});
                return;
            }
            std::vector<uint8_t> rb(parsed->payload_length);
            if (!job->socket.receive_all(rb.data(), rb.size()).has_value() ||
                !upix::Protocol::verify_frame(rhdr.data(), rb.data(), rb.size())) {
                finish(1, {});
                return;
            }
            upix::ServeResponse resp{};
            const size_t struct_bytes = std::min<size_t>(rb.size(), sizeof(resp));
            std::memcpy(&resp, rb.data(), struct_bytes);
            // §4 tolerance: a same-repo older daemon omits the skipped tail —
            // its frame is 8 bytes shorter than the path payload suggests.
            if (rb.size() < sizeof(resp) + static_cast<size_t>(resp.out_path_length)) {
                resp.skipped = 0;
                resp.reserved0 = 0;
            }
            if (!resp.ok) {
                finish(1, QString::fromLatin1(resp.error));
                return;
            }
            if (resp.skipped) {
                emitLine(QStringLiteral(R"({"t":"note","message":"已是超分结果,已跳过"})"));
                finish(0, {});
                return;
            }
            const size_t path_len = std::min<size_t>(
                resp.out_path_length, rb.size() - struct_bytes);
            const QString saved = QString::fromUtf8(
                reinterpret_cast<const char*>(rb.data()) + struct_bytes,
                static_cast<qsizetype>(path_len));
            emitLine(QStringLiteral(R"({"t":"note","message":"%1x%2"})")
                         .arg(resp.width).arg(resp.height));
            emitLine(QStringLiteral(R"({"t":"saved","path":"%1"})").arg(saved));
            finish(0, {});
            return;
        }
    });
}

void Desk::onServeDone(int code, const QString& detail) {
    ServeJob* job = m_serveJob;
    m_serveJob = nullptr;
    delete job;
    if (code == 2) {
        // No daemon answered: the same settings run through the share batch
        // core with its own engine (same card state — the card was reset
        // before the gate).
        startBatchJob();
        return;
    }
    m_jobOutput = detail.toUtf8();
    finishJob(code);
}

namespace {
// JSON lines for the job stream, built with QJson — filenames are user data
// and must be escaped by a real parser, not string surgery.
template <typename...Pairs>
QString machineLine(Pairs&&... pairs) {
    QJsonObject o;
    (o.insert(pairs.first, pairs.second), ...);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
}  // namespace

void Desk::startBatchJob() {
    const QVariantList inputs = m_runSnap.value("inputs").toList();
    const QVariantMap input = inputs.at(m_runIndex).toMap();
    const QVariantMap settings = m_runSnap;
    const std::string path =
        std::filesystem::absolute(
            std::filesystem::path(input.value("path").toString().toStdString()))
            .lexically_normal().string();
    const bool isDir = input.value("dir").toBool();
    const QString model = settings.value("model").toString();

    auto job = new BatchJob(this);
    m_batchJob = job;
    upix::BatchRequest& req = job->request;
    req.models_dir = upix::default_models_dir();
    req.model_name = model.toStdString();

    // Same effective config the CLI job carried (no config file, explicit
    // tile): auto-tile on, or a pinned size; never the engine probe.
    upix::ProcessingConfig& cfg = req.config;
    const QString fmt = settings.value("format").toString();
    if (!fmt.isEmpty())
        cfg.output_format = upix::normalize_output_format(fmt.toStdString());
    cfg.output_quality = settings.value("quality").toInt();
    cfg.tile_size = settings.value("tileSize").toInt();
    cfg.tile_overlap = upix::kDefaultTileOverlap;
    cfg.auto_tile_size = settings.value("autoTile").toBool();
    req.use_gpu = settings.value("gpu").toBool();
    cfg.use_gpu = req.use_gpu;
    req.force = settings.value("force").toBool();
    req.jobs = settings.value("jobs").toInt();
    // CLI parity: `-j` only ever shipped for CPU directories; GPU dirs let
    // the VRAM admission control decide (run_batch, jobs_explicit=false).
    req.jobs_explicit = isDir && !req.use_gpu;

    std::string error;
    if (isDir) {
        const std::string outDir =
            upix::derive_default_output(path, req.model_name, true);
        auto tasks = upix::gather_directory_tasks(path, outDir, error);
        if (!tasks.has_value()) {
            onBatchDone(1, QString::fromStdString(error));
            return;
        }
        req.tasks = std::move(*tasks);
    } else {
        const std::string out = upix::derive_default_output(path, req.model_name, false);
        req.tasks.emplace_back(path, out);
    }

    job->thread = std::thread([this, job] {
        auto emitLine = [this, job](QString line) {
            if (!job->desk) return;
            QMetaObject::invokeMethod(job->desk,
                                      [this, line] { handleJobLine(line); },
                                      Qt::QueuedConnection);
        };
        auto finish = [this, job](int code, QString detail) {
            if (job->done.exchange(true)) return;
            if (!job->desk) return;
            QMetaObject::invokeMethod(job->desk,
                                      [this, code, detail] { onBatchDone(code, detail); },
                                      Qt::QueuedConnection);
        };

        upix::BatchCallbacks cb;
        cb.on_current = [&emitLine](const std::string& file) {
            emitLine(machineLine(
                qMakePair(QStringLiteral("t"), QStringLiteral("current")),
                qMakePair(QStringLiteral("file"),
                          QString::fromStdString(file))));
        };
        cb.on_saved = [&emitLine](const std::string& out) {
            emitLine(machineLine(
                qMakePair(QStringLiteral("t"), QStringLiteral("saved")),
                qMakePair(QStringLiteral("path"),
                          QString::fromStdString(out))));
        };
        cb.on_skipped = [&emitLine](const std::string&) {
            emitLine(machineLine(
                qMakePair(QStringLiteral("t"), QStringLiteral("note")),
                qMakePair(QStringLiteral("message"),
                          QStringLiteral("已是超分结果,已跳过"))));
        };
        cb.on_progress = [&emitLine](const upix::ProcessingProgress& prog,
                                     int eta) {
            if (prog.total_tiles <= 0) return;
            const int pct = std::min(
                100, static_cast<int>(prog.completed_tiles) * 100 /
                         static_cast<int>(prog.total_tiles));
            emitLine(machineLine(
                qMakePair(QStringLiteral("t"), QStringLiteral("progress")),
                qMakePair(QStringLiteral("pct"), pct),
                qMakePair(QStringLiteral("done"),
                          static_cast<int>(prog.completed_tiles)),
                qMakePair(QStringLiteral("total"),
                          static_cast<int>(prog.total_tiles)),
                qMakePair(QStringLiteral("eta"), std::max(0, eta))));
        };

        auto stats = upix::run_batch(job->request, cb, job->cancelled);
        if (!stats.has_value()) {
            finish(1, {});
            return;
        }
        finish(stats->succeeded + stats->skipped == stats->total ? 0 : 1, {});
    });
}

void Desk::onBatchDone(int code, const QString& detail) {
    BatchJob* job = m_batchJob;
    m_batchJob = nullptr;
    delete job;
    m_jobOutput = detail.toUtf8();
    finishJob(code);
}

void Desk::daemonSpawnSelf() {
    const std::string socket = upix::default_serve_socket_path();
    if (socket.empty()) {
        setNotice(QStringLiteral("无法定位 daemon socket 路径"));
        return;
    }
    if (upix::serve_daemon_status(socket).running) {
        refreshDaemonStatus();
        return;
    }
    // Re-exec THIS binary headlessly (docs/SPEC-frontends.md stage 2): the
    // daemonized child runs the same core, no CLI involved. The pid file is
    // written by the child after a successful start, so `daemon stop`/status
    // semantics stay identical to the CLI-managed daemon.
    const QString exe = QCoreApplication::applicationFilePath();
    std::vector<std::string> args{exe.toStdString(),
                                  "--serve-daemon",
                                  "--socket", socket,
                                  "--models-dir", upix::default_models_dir()};
    const QString model = s()["model"].toString();
    if (!model.isEmpty()) {
        args.push_back("--model");
        args.push_back(model.toStdString());
    }
    args.push_back("--idle-timeout");
    args.push_back("600");
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        setNotice(QStringLiteral("daemon 启动失败: fork"));
        return;
    }
    if (pid == 0) {
        ::setsid();
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
        ::execv(args[0].c_str(), argv.data());
        _exit(127);  // exec failed — never unwind back into Qt
    }
    // Engine init + model load take seconds before the pid file appears;
    // probe twice so the panel settles without a manual refresh.
    QTimer::singleShot(2500, this, [this] { refreshDaemonStatus(); });
    QTimer::singleShot(7000, this, [this] { refreshDaemonStatus(); });
}

void Desk::refreshDaemonStatus() {
#ifdef Q_OS_UNIX
    const std::string socket = upix::default_serve_socket_path();
    QVariantMap daemon;
    if (socket.empty()) {
        daemon["running"] = false;
        daemon["pid"] = 0;
        daemon["socketName"] = QString();
        daemon["socketMissing"] = false;
        daemon["label"] = QStringLiteral("未运行");
        s()["daemon"] = daemon;
        touch();
        return;
    }
    const auto st = upix::serve_daemon_status(socket);
    QVariantMap doc;
    doc["running"] = st.running;
    doc["stale"] = st.stale;
    doc["pid"] = static_cast<int>(st.pid);
    doc["socket"] = QString::fromStdString(st.socket_path);
    doc["socketMissing"] = st.socket_missing;
    daemon["running"] = st.running;
    daemon["pid"] = static_cast<int>(st.pid);
    daemon["socketName"] = st.running
        ? QFileInfo(QString::fromStdString(st.socket_path)).fileName()
        : QString();
    daemon["socketMissing"] = st.socket_missing;
    daemon["label"] = machine::daemonLabel(doc);
    s()["daemon"] = daemon;
    touch();
#else
    QProcess* p = spawn(QStringLiteral("dstatus"),
                        {QStringLiteral("daemon"), QStringLiteral("status"), QStringLiteral("--json")});
    if (!p) return;
    connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
        // Exit code 1 = not running; the document is the source of truth.
        const QVariantMap d = oneShotDoc(p);
        QVariantMap daemon;
        daemon["running"] = d.value(QStringLiteral("running")).toBool();
        daemon["pid"] = d.value(QStringLiteral("pid")).toInt();
        const QString socket = d.value(QStringLiteral("socket")).toString();
        daemon["socketName"] = socket.isEmpty() ? QString() : QFileInfo(socket).fileName();
        daemon["socketMissing"] = d.value(QStringLiteral("socketMissing")).toBool();
        daemon["label"] = machine::daemonLabel(d);
        s()["daemon"] = daemon;
        touch();
    });
#endif
}

void Desk::daemonAction(const QString& action) {
#ifdef Q_OS_UNIX
    if (action == QLatin1String("start")) {
        daemonSpawnSelf();
        return;
    }
    if (action == QLatin1String("stop")) {
        // serve_daemon_stop waits up to ~3 s for the accept loop to unwind:
        // off the GUI thread, refresh queued when it settles.
        std::thread([this] {
            upix::serve_daemon_stop(upix::default_serve_socket_path());
            QMetaObject::invokeMethod(this, [this] { refreshDaemonStatus(); },
                                      Qt::QueuedConnection);
        }).detach();
        return;
    }
    refreshDaemonStatus();
#else
    QProcess* p = spawn(QStringLiteral("daemon"), {QStringLiteral("daemon"), action});
    if (!p) return;
    connect(p, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        refreshDaemonStatus();
    });
#endif
}

void Desk::handleHostEvent(const QString& kind, const QVariantMap& data) {
    QVariantMap h = s()["host"].toMap();
    bool dirty = true;
    if (kind == QLatin1String("card")) {
        h["fingerprint"] = data.value(QStringLiteral("fingerprint"));
        h["ips"] = data.value(QStringLiteral("ips"));
        const int port = data.value(QStringLiteral("port")).toInt();
        if (port > 0) h["port"] = port;
        h["qr"] = data.value(QStringLiteral("qr"));
        h["running"] = true;
        QVariantList log = h["log"].toList();
        log.append(QStringLiteral("Coordinator running."));
        h["log"] = log;
    } else if (kind == QLatin1String("pairing")) {
        h["pendingDevice"] = data.value(QStringLiteral("device"));
        h["pendingFp"] = data.value(QStringLiteral("fingerprint"));
    } else if (kind == QLatin1String("task")) {
        h["taskPercent"] = data.value(QStringLiteral("pct"));
    } else if (kind == QLatin1String("log")) {
        QVariantList log = h["log"].toList();
        log.append(data.value(QStringLiteral("line")));
        while (log.size() > 50) log.removeFirst();
        h["log"] = log;
    } else if (kind == QLatin1String("error")) {
        setNotice(data.value(QStringLiteral("message")).toString());
        hostReset();
        return;
    } else if (kind == QLatin1String("finished")) {
        const bool user = data.value(QStringLiteral("stoppedByUser")).toBool();
        m_hostStopping = false;
        hostReset();
        if (!user)
            setNotice(QStringLiteral("主机已退出"));
        refreshDaemonStatus();
        return;
    } else {
        dirty = false;
    }
    if (dirty) {
        s()["host"] = h;
        touch();
    }
}

void Desk::hostStart() {
#ifdef Q_OS_UNIX
    if (m_hostRun) return;  // the coordinator is already up / coming up
    hostReset();
    m_hostStopping = false;
    const int port = s()["host"].toMap().value("port").toInt();
    auto run = new HostRun(this);
    m_hostRun = run;
    QVariantMap h = s()["host"].toMap();
    h["running"] = true;  // "starting" — the card event flips the panel live
    s()["host"] = h;
    touch();

    run->thread = std::thread([this, run, port] {
        auto emitHost = [this, run](const QString& kind, const QVariantMap& data) {
            if (!run->desk) return;
            QMetaObject::invokeMethod(run->desk,
                                      [this, run, kind, data] { handleHostEvent(kind, data); },
                                      Qt::QueuedConnection);
        };

        auto identity = upix::IdentityKey::load_or_create(upix::default_identity_path());
        run->coordinator.set_models_directory(upix::default_models_dir());
        run->coordinator.set_trust_store_path(upix::default_trust_path());
        run->coordinator.on_task_progress() = [run](float progress) {
            run->taskPct.store(progress < 0 ? -1
                                            : static_cast<int>(progress * 100));
        };
        auto started = run->coordinator.start(static_cast<uint16_t>(port), identity);
        if (!started.has_value()) {
            QVariantMap d;
            d["message"] = QStringLiteral("主机启动失败: %1")
                               .arg(QString::fromStdString(started.error().message));
            emitHost(QStringLiteral("error"), d);
            return;
        }

        const upix::ConnectionCard card =
            upix::build_connection_card(static_cast<uint16_t>(port),
                                        identity.public_key());
        QVariantMap cardData;
        cardData["fingerprint"] = QString::fromStdString(card.fingerprint);
        QVariantList ips;
        for (const auto& a : card.addresses)
            if (!ips.contains(QString::fromStdString(a)))
                ips.append(QString::fromStdString(a));
        cardData["ips"] = ips;
        cardData["port"] = port;
        cardData["qr"] = QString::fromStdString(card.qr_payload);
        emitHost(QStringLiteral("card"), cardData);

        int lastPct = -1;
        while (!run->stopping.load(std::memory_order_relaxed) &&
               run->coordinator.is_running()) {
            // First-time pairing gate: surface the request, the user decides.
            auto pend = run->coordinator.pop_pending_approval();
            if (pend.has_value()) {
                {
                    std::lock_guard<std::mutex> lock(run->pendingMutex);
                    run->pending = pend;
                }
                QVariantMap d;
                d["device"] = QString::fromStdString(pend->device_name);
                d["fingerprint"] = QString::fromStdString(pend->fingerprint);
                emitHost(QStringLiteral("pairing"), d);
            }
            const int pct = run->taskPct.load(std::memory_order_relaxed);
            if (pct != lastPct) {
                lastPct = pct;
                QVariantMap d;
                d["pct"] = pct;
                emitHost(QStringLiteral("task"), d);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        QVariantMap d;
        d["stoppedByUser"] = run->stopping.load(std::memory_order_relaxed);
        emitHost(QStringLiteral("finished"), d);
    });
#else
    if (!s()["cliFound"].toBool()) return;
    // A second click while the coordinator is still coming up: spawn() would
    // kill the very process the user is waiting on and leave the panel to
    // time out on its own.
    if (QProcess* live = m_procs.value(QStringLiteral("host"));
        live && live->state() != QProcess::NotRunning)
        return;
    hostReset();
    m_hostStopping = false;
    QProcess* process = spawn(QStringLiteral("host"),
                              {QStringLiteral("host"), QStringLiteral("start"),
                               QStringLiteral("--json"),
                               QStringLiteral("-p"), QString::number(s()["host"].toMap().value("port").toInt())});
    if (!process) {
        setNotice(QStringLiteral("无法启动主机"));
        return;
    }
    // The coordinator prints its card (QR) a moment later; without this the
    // panel sat on "未运行" with an enabled 启动主机 button, and the guard above
    // then swallowed the second click. `running` here means "process is up".
    {
        QVariantMap h = s()["host"].toMap();
        h["running"] = true;
        s()["host"] = h;
        touch();
    }
    connect(process, &QProcess::readyRead, this, [this, process] { readHostOutput(process); });
    connect(process, &QProcess::finished, this,
            [this, process](int code, QProcess::ExitStatus status) {
                readHostOutput(process, true);
                // Exit code is the only signal that host start failed (port in
                // use, CA missing); without a notice the panel just reset.
                const bool failed = code != 0 || status != QProcess::NormalExit;
                const bool stoppedByUser = m_hostStopping;
                m_hostStopping = false;
                hostReset();
                if (failed && !stoppedByUser)
                    setNotice(QStringLiteral("主机已退出（code %1）").arg(code));
                refreshDaemonStatus();
            });
#endif
}

void Desk::hostStop() {
#ifdef Q_OS_UNIX
    if (m_hostRun) {
        m_hostStopping = true;  // this exit is the user's, not a failure
        m_hostRun->requestStop();  // the worker posts "finished"
        return;
    }
    m_hostPending.clear();
    hostReset();
#else
    QProcess* p = m_procs.value(QStringLiteral("host"));
    if (p && p->state() != QProcess::NotRunning) {
        m_hostStopping = true;  // this exit is the user's, not a failure
        p->terminate();
        // Same escalation as cancel(): the panel must not claim "未运行" while
        // a wedged host still holds the port.
        QPointer<QProcess> guard(p);
        QTimer::singleShot(2000, this, [guard] {
            if (guard && guard->state() != QProcess::NotRunning) guard->kill();
        });
    }
    m_hostPending.clear();
    hostReset();
#endif
}

void Desk::hostDecide(bool allow) {
#ifdef Q_OS_UNIX
    if (m_hostRun) m_hostRun->decide(allow);
#else
    QProcess* p = m_procs.value(QStringLiteral("host"));
    if (p && p->state() == QProcess::Running &&
        !s()["host"].toMap()["pendingDevice"].toString().isEmpty()) {
        p->write(allow ? "y\n" : "n\n");
    }
#endif
    QVariantMap h = s()["host"].toMap();
    h["pendingDevice"] = QString();
    h["pendingFp"] = QString();
    s()["host"] = h;
    touch();
}

#endif  // Q_OS_UNIX

void Desk::readJobOutput(QProcess* process, bool final) {
    const QByteArray bytes = process->readAll();
    m_jobPending += bytes;
    int newline = -1;
    while ((newline = m_jobPending.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_jobPending.left(newline)).remove(QLatin1Char('\r'));
        m_jobPending.remove(0, newline + 1);
        handleJobLine(line);
    }
    if (final && !m_jobPending.isEmpty()) {
        m_jobPending.append('\n');
        readJobOutput(process, false);
    }
}

void Desk::handleJobLine(const QString& line) {
    const QVariantMap event = machine::parseEvent(line);
    if (event.isEmpty()) {
        // Human log line (stderr in machine mode): failure diagnostics only.
        const QString text = line.trimmed();
        if (!text.isEmpty()) {
            m_jobOutput += line.toUtf8();
            m_jobOutput += '\n';
        }
        return;
    }
    QVariantMap job = s()["job"].toMap();
    const QString type = event.value(QStringLiteral("t")).toString();
    bool dirty = false;
    if (type == QLatin1String("progress")) {
        job["percent"] = event.value(QStringLiteral("pct")).toInt();
        job["tilesDone"] = event.value(QStringLiteral("done")).toInt();
        job["tilesTotal"] = event.value(QStringLiteral("total")).toInt();
        job["etaSec"] = event.value(QStringLiteral("eta")).toInt();
        dirty = true;
    } else if (type == QLatin1String("current")) {
        job["current"] = event.value(QStringLiteral("file")).toString();
        dirty = true;
    } else if (type == QLatin1String("saved")) {
        QVariantList outputs = job["outputs"].toList();
        outputs.append(QUrl::fromLocalFile(event.value(QStringLiteral("path")).toString()).toString());
        job["outputs"] = outputs;
        dirty = true;
    } else if (type == QLatin1String("note")) {
        job["message"] = machine::doneNote(event.value(QStringLiteral("message")).toString());
        dirty = true;
    }
    if (dirty) { s()["job"] = job; touch(); }
}

void Desk::finishJob(int code) {
    QProcess* process = m_procs.take(QStringLiteral("job"));
    const QString detail = QString::fromUtf8(m_jobOutput).trimmed();
    const QString processError = process ? process->errorString() : QString();
    if (process) {
        process->disconnect(this);
        process->deleteLater();
    }
    QVariantMap job = s()["job"].toMap();
    if (job.value("phase").toString() != QLatin1String("running")) return;

    if (m_cancelRequested) {
        job["phase"] = QStringLiteral("cancelled");
        job["message"] = QStringLiteral("已取消");
        s()["job"] = job;
        touch();
        return;
    }
    if (code == 0) {
        ++m_runIndex;
        startNextJob();
        return;
    }
    job["phase"] = QStringLiteral("error");
    job["message"] = !detail.isEmpty() ? detail
                    : !processError.isEmpty() ? processError
                    : QStringLiteral("处理失败");
    s()["job"] = job;
    setNotice(job.value("message").toString());
    touch();
    refreshDaemonStatus();
}

void Desk::cancel() {
    if (s()["job"].toMap().value("phase").toString() != QLatin1String("running")) return;
    m_cancelRequested = true;
#ifdef Q_OS_UNIX
    // Wake whichever worker owns the run; both report back through their
    // done events and the m_cancelRequested branch of finishJob closes the
    // card out. The batch core checks the flag at every queue hand-off and
    // tile boundary, so even a huge directory stops within seconds.
    if (m_serveJob) {
        m_serveJob->requestCancel();
        return;
    }
    if (m_batchJob) {
        m_batchJob->requestCancel();
        return;
    }
#endif
    QProcess* process = m_procs.value(QStringLiteral("job"));
    if (!process || process->state() != QProcess::Running) {
        QVariantMap job = s()["job"].toMap();
        job["phase"] = QStringLiteral("cancelled");
        job["message"] = QStringLiteral("已取消");
        s()["job"] = job;
        touch();
        return;
    }
    process->terminate();
    QPointer<QProcess> guard(process);
    QTimer::singleShot(2000, this, [guard] {
        if (guard && guard->state() == QProcess::Running) guard->kill();
    });
}

// ---- models ------------------------------------------------------------

namespace {
QVariantMap oneShotDoc(QProcess* p) {
    const QJsonDocument doc = QJsonDocument::fromJson(QString::fromUtf8(p->readAll()).toUtf8());
    return doc.object().toVariantMap();
}
}  // namespace

void Desk::refreshModels() {
#ifdef Q_OS_UNIX
    // Stage 2: scan + remote catalog straight from share. The installed list
    // lands before the (slower, network) remote catalog so the page paints
    // usable content immediately.
    std::thread([this] {
        const std::string dir = upix::default_models_dir();
        QVariantList installed;
        QVariantList installedGroups;
        QString scanError;
        {
            upix::ModelManager manager(dir);
            auto models = manager.scan_models();
            if (models.has_value()) {
                for (const auto& m : *models) {
                    const auto meta = upix::classify_model_scene(m.name);
                    installed.append(QVariantMap{
                        {QStringLiteral("name"), QString::fromStdString(m.name)},
                        {QStringLiteral("scale"), m.scale_factor},
                        {QStringLiteral("size"),
                         QString::number(static_cast<double>(m.total_size) / 1048576.0,
                                         'f', 0) + QStringLiteral(" MB")},
                        {QStringLiteral("scene"), QString::fromStdString(std::string(meta.scene))},
                        {QStringLiteral("recommended"), meta.recommended}});
                }
                installedGroups = groupedCatalog(installed);
            } else {
                scanError = QString::fromStdString(models.error().message);
            }
        }
        QMetaObject::invokeMethod(this, [this, dir, installed, installedGroups, scanError] {
            if (!scanError.isEmpty()) {
                s()["remoteError"] = scanError;
            } else {
                s()["modelsDir"] = QString::fromStdString(dir);
                s()["installed"] = installed;
                s()["installedGroups"] = installedGroups;
            }
            touch();
        }, Qt::QueuedConnection);

        std::string remoteError;
        QVariantList remote;
        if (auto r = upix::fetch_remote_models(std::string(), remoteError)) {
            for (const auto& m : *r) {
                const auto meta = upix::classify_model_scene(m.name);
                remote.append(QVariantMap{
                    {QStringLiteral("name"), QString::fromStdString(m.name)},
                    {QStringLiteral("scale"), m.scale},
                    {QStringLiteral("size"),
                     QString::number(static_cast<double>(m.size_bytes) / 1048576.0,
                                     'f', 1) + QStringLiteral(" MB")},
                    {QStringLiteral("scene"), QString::fromStdString(std::string(meta.scene))},
                    {QStringLiteral("recommended"), meta.recommended}});
            }
            remoteError.clear();
        }
        QMetaObject::invokeMethod(this, [this, remote,
                                         err = QString::fromStdString(remoteError)] {
            s()["remoteError"] = err;
            s()["remote"] = remote;
            touch();
        }, Qt::QueuedConnection);
    }).detach();
#else
    QProcess* p = spawn(QStringLiteral("models"),
                        {QStringLiteral("model"), QStringLiteral("list"), QStringLiteral("--json")});
    if (!p) {
        // Without a CLI the catalog used to sit on "正在获取" forever.
        s()["remoteError"] = QStringLiteral("无法启动 upix-cli");
        touch();
        return;
    }
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        if (code != 0) {
            s()["remoteError"] = QStringLiteral("本地模型列表获取失败");
            touch();
            return;
        }
        const QVariantMap m = oneShotDoc(p);
        s()["modelsDir"] = m.value(QStringLiteral("modelsDir"));
        const QVariantList installed = m.value(QStringLiteral("installed")).toList();
        s()["installed"] = installed;
        s()["installedGroups"] = groupedCatalog(installed);
        touch();

        QProcess* r = spawn(QStringLiteral("remote"),
                            {QStringLiteral("model"), QStringLiteral("list-remote"),
                             QStringLiteral("--json")});
        if (!r) {
            s()["remoteError"] = QStringLiteral("无法启动 upix-cli");
            touch();
            return;
        }
        connect(r, &QProcess::finished, this, [this, r](int code, QProcess::ExitStatus) {
            if (code != 0) {
                s()["remoteError"] = QStringLiteral("远端列表获取失败");
                touch();
                return;
            }
            s()["remoteError"] = QString();
            s()["remote"] = oneShotDoc(r).value(QStringLiteral("remote"));
            touch();
        });
    });
#endif
}

void Desk::importModels(const QStringList& paths) {
    if (paths.isEmpty()) return;
#ifdef Q_OS_UNIX
    std::thread([this, paths] {
        std::vector<std::string> files;
        for (const QString& p : paths) files.push_back(p.toStdString());
        auto result = upix::import_model_files(files, upix::default_models_dir());
        QMetaObject::invokeMethod(this, [this, result] {
            if (result.ok) {
                setNotice(QStringLiteral("导入完成"));
                refreshModels();
            } else {
                setNotice(result.error.empty() ? QStringLiteral("导入失败")
                                               : QString::fromStdString(result.error));
            }
        }, Qt::QueuedConnection);
    }).detach();
#else
    QProcess* p = spawn(QStringLiteral("import"),
                        QStringList{QStringLiteral("model"), QStringLiteral("import"),
                                    QStringLiteral("--json")} + paths);
    if (!p) {
        setNotice(s()["cliFound"].toBool() ? QStringLiteral("无法启动 upix-cli")
                                           : QStringLiteral("未找到 upix-cli"));
        return;
    }
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        const QVariantMap m = oneShotDoc(p);
        if (code == 0 && m.value(QStringLiteral("ok")).toBool()) {
            setNotice(QStringLiteral("导入完成"));
            refreshModels();
        } else {
            const QString error = m.value(QStringLiteral("error")).toString();
            setNotice(error.isEmpty() ? QStringLiteral("导入失败") : error);
        }
    });
#endif
}

namespace {
// Case/dash-insensitive manifest match (same idea as the CLI's find_remote;
// exact matches are the normal path — names come from the same manifest).
auto findRemote(const std::vector<upix::RemoteModel>& models,
                const std::string& name) -> const upix::RemoteModel* {
    auto norm = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) {
                    return c == '-' || c == '_' || c == ' ';
                }), s.end());
        return s;
    };
    const std::string want = norm(name);
    for (const auto& m : models)
        if (norm(m.name) == want) return &m;
    return nullptr;
}
}  // namespace

void Desk::download(const QString& name) {
    if (s()["downloading"].toString() == name) return;
#ifdef Q_OS_UNIX
    s()["downloading"] = name;
    s()["downloadPct"] = 0;
    touch();
    std::thread([this, name] {
        auto post = [this](std::function<void()> body) {
            QMetaObject::invokeMethod(this, [this, body] { body(); },
                                      Qt::QueuedConnection);
        };
        std::string error;
        auto remote = upix::fetch_remote_models(std::string(), error);
        const upix::RemoteModel* m =
            remote.has_value() ? findRemote(*remote, name.toStdString()) : nullptr;
        if (!m) {
            post([this, name] {
                s()["downloading"] = QString();
                s()["downloadPct"] = -1;
                setNotice(QStringLiteral("下载失败（清单里找不到）: ") + name);
                touch();
            });
            return;
        }
        auto result = upix::download_model(*m, upix::default_models_dir(),
            [this, name](const upix::DownloadProgress& p) {
                if (p.total <= 0) return;
                const int pct = static_cast<int>(p.downloaded * 100 / p.total);
                QMetaObject::invokeMethod(this, [this, pct] {
                    if (pct >= 0 && pct != s()["downloadPct"].toInt()) {
                        s()["downloadPct"] = pct;
                        touch();
                    }
                }, Qt::QueuedConnection);
            });
        post([this, name, result] {
            s()["downloading"] = QString();
            s()["downloadPct"] = -1;
            if (result.ok) {
                setNotice(QStringLiteral("已下载 ") + name);
                refreshModels();
            } else {
                setNotice(QStringLiteral("下载失败: ") + name +
                          (result.message.empty()
                               ? QString()
                               : QStringLiteral(" (%1)").arg(
                                     QString::fromStdString(result.message))));
            }
            touch();
        });
    }).detach();
#else
    QProcess* p = spawn(QStringLiteral("download"),
                        {QStringLiteral("model"), QStringLiteral("download"),
                         QStringLiteral("--json"), name});
    if (!p) {
        setNotice(s()["cliFound"].toBool() ? QStringLiteral("无法启动 upix-cli")
                                           : QStringLiteral("未找到 upix-cli"));
        return;
    }
    s()["downloading"] = name;
    s()["downloadPct"] = 0;
    touch();
    connect(p, &QProcess::readyRead, this, [this, p, name] { readDownloadOutput(p, name); });
    connect(p, &QProcess::finished, this, [this, name](int code, QProcess::ExitStatus) {
        s()["downloading"] = QString();
        s()["downloadPct"] = -1;
        if (code == 0) {
            setNotice(QStringLiteral("已下载 ") + name);
            refreshModels();
        } else {
            setNotice(QStringLiteral("下载失败: ") + name);
        }
    });
#endif
}

void Desk::readDownloadOutput(QProcess* process, const QString& name) {
    m_downloadPending += process->readAll();
    int newline = -1;
    while ((newline = m_downloadPending.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_downloadPending.left(newline));
        m_downloadPending.remove(0, newline + 1);
        const QVariantMap event = machine::parseEvent(line);
        if (event.value(QStringLiteral("t")).toString() != QLatin1String("download")) continue;
        // Multiple queued names share one stream; only the tracked one drives the bar.
        if (event.value(QStringLiteral("name")).toString() != name) continue;
        const int pct = event.value(QStringLiteral("pct")).toInt();
        if (pct >= 0 && pct != s()["downloadPct"].toInt()) {
            s()["downloadPct"] = pct;
            touch();
        }
    }
}

// ---- daemon ------------------------------------------------------------

void Desk::readHostOutput(QProcess* process, bool final) {
    m_hostPending += process->readAll();
    QStringList lines;
    int newline = -1;
    while ((newline = m_hostPending.indexOf('\n')) >= 0) {
        lines.append(QString::fromUtf8(m_hostPending.left(newline)).remove(QLatin1Char('\r')));
        m_hostPending.remove(0, newline + 1);
    }
    if (final && !m_hostPending.isEmpty()) {
        lines.append(QString::fromUtf8(m_hostPending).remove(QLatin1Char('\r')));
        m_hostPending.clear();
    }

    QVariantMap host = s()["host"].toMap();
    bool dirty = false;
    for (const QString& line : lines) {
        const QVariantMap event = machine::parseEvent(line);
        if (event.isEmpty()) {
            // Human log line (stderr in machine mode) → the log strip.
            if (line.contains(QStringLiteral("[INFO]"))) {
                QVariantList log = host["log"].toList();
                log.append(line.section(QStringLiteral("] "), 2));
                while (log.size() > 50) log.removeFirst();
                host["log"] = log;
                dirty = true;
            }
            continue;
        }
        const QString type = event.value(QStringLiteral("t")).toString();
        if (type == QLatin1String("card")) {
            host["fingerprint"] = event.value(QStringLiteral("fingerprint"));
            const int port = event.value(QStringLiteral("port")).toInt();
            if (port > 0) host["port"] = port;
            QVariantList ips = host["ips"].toList();
            for (const QVariant& ip : event.value(QStringLiteral("ips")).toList())
                if (!ips.contains(ip)) ips.append(ip);
            host["ips"] = ips;
            // v2 invite payload — image://qr renders it directly (no more
            // half-block art re-parse).
            host["qr"] = event.value(QStringLiteral("qr"));
            host["running"] = true;
            dirty = true;
        } else if (type == QLatin1String("pairing")) {
            host["pendingDevice"] = event.value(QStringLiteral("device"));
            host["pendingFp"] = event.value(QStringLiteral("fingerprint"));
            dirty = true;
        } else if (type == QLatin1String("task")) {
            host["taskPercent"] = event.value(QStringLiteral("pct")).toInt();
            dirty = true;
        }
    }
    if (dirty) { s()["host"] = host; touch(); }
}

void Desk::setHostPort(int port) {
    if (port <= 0 || port > 65535) return;
    QVariantMap h = s()["host"].toMap();
    h["port"] = port;
    s()["host"] = h;
    savePrefs();
    touch();
}

void Desk::hostReset() {
    QVariantMap h = defaultSnap().value("host").toMap();
    h["port"] = s()["host"].toMap().value("port");
    s()["host"] = h;
    touch();
}

void Desk::copyFingerprint() {
    const QString fp = s()["host"].toMap().value("fingerprint").toString();
    if (fp.isEmpty()) return;
    QGuiApplication::clipboard()->setText(fp);
    setNotice(QStringLiteral("指纹已复制"));
}

void Desk::reveal(const QString& path) {
    QString target = path;
    if (target.startsWith(QStringLiteral("file://"))) target = QUrl(target).toLocalFile();
    if (target.isEmpty()) {
        const QVariantList outs = s()["job"].toMap().value("outputs").toList();
        if (!outs.isEmpty()) target = QUrl(outs.last().toString()).toLocalFile();
        else if (!s()["inputs"].toList().isEmpty())
            target = s()["inputs"].toList().first().toMap().value("path").toString();
    }
    if (target.isEmpty()) return;
    const QFileInfo info(target);
    const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (!dir.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void Desk::openPath(const QString& path) {
    QString target = path;
    if (target.startsWith(QStringLiteral("file://"))) target = QUrl(target).toLocalFile();
    if (target.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}

// Ctrl+V: accept image URLs or a raw bitmap from any app. A raw bitmap is
// stashed in CacheLocation and added like a picked file.
void Desk::pasteClipboard() {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime && mime->hasUrls()) {
        QStringList paths;
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& u : urls)
            paths.append(u.isLocalFile() ? u.toLocalFile() : u.toString());
        addPaths(paths);
        return;
    }
    const QImage img = QGuiApplication::clipboard()->image();
    if (img.isNull()) {
        setNotice(QStringLiteral("剪贴板里没有图片"));
        return;
    }
    QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    if (!cache.exists()) cache.mkpath(QStringLiteral("."));
    const QString path =
        cache.filePath(QStringLiteral("paste-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!img.save(path)) {
        setNotice(QStringLiteral("粘贴失败: 无法写入缓存"));
        return;
    }
    addPaths({path});
}

void Desk::applyFakeSnapshot(const QVariantMap& snap) {
    m_snap = snap;
    notifyNow();
}

QVariantMap Desk::fakeSnapshot(const QString& state) {
    return fakeSnap(state);
}
