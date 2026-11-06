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

namespace {

constexpr int CoalesceMs = 70;
constexpr int PrefsSaveMs = 400;

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
    s["installed"] = QVariantList{
        QVariantMap{{"name", QStringLiteral("Real-ESRGANv3-anime-x2")}, {"scale", 2}, {"size", QStringLiteral("1 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", true}},
        QVariantMap{{"name", QStringLiteral("AnimeSharp-x4")}, {"scale", 4}, {"size", QStringLiteral("31 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", true}},
        QVariantMap{{"name", QStringLiteral("Real-CUGAN-denoise-x2")}, {"scale", 2}, {"size", QStringLiteral("2 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", false}},
        QVariantMap{{"name", QStringLiteral("Remacri-x4")}, {"scale", 4}, {"size", QStringLiteral("31 MB")}, {"scene", QStringLiteral("综合")}, {"recommended", true}},
    };
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

void Desk::boot() {
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
}

QProcess* Desk::spawn(const QString& key, const QStringList& args,
                      const QStringList& env) {
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
        s()["installed"] = m.value(QStringLiteral("installed"));
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
}

void Desk::importModels(const QStringList& paths) {
    if (paths.isEmpty()) return;
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
}

void Desk::download(const QString& name) {
    if (s()["downloading"].toString() == name) return;
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

void Desk::refreshDaemonStatus() {
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
}

void Desk::daemonAction(const QString& action) {
    QProcess* p = spawn(QStringLiteral("daemon"), {QStringLiteral("daemon"), action});
    if (!p) return;
    connect(p, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        refreshDaemonStatus();
    });
}

// ---- host --------------------------------------------------------------

void Desk::hostReset() {
    QVariantMap h = defaultSnap().value("host").toMap();
    h["port"] = s()["host"].toMap().value("port");
    s()["host"] = h;
    touch();
}

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

void Desk::hostStart() {
    if (!s()["cliFound"].toBool()) return;
    // A second click while the coordinator is still coming up: spawn() would
    // kill the very process the user is waiting on and leave the panel to
    // time out on its own.
    if (QProcess* live = m_procs.value(QStringLiteral("host"));
        live && live->state() != QProcess::NotRunning)
        return;
    hostReset();
    m_hostPending.clear();
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
}

void Desk::hostStop() {
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
}

void Desk::setHostPort(int port) {
    if (port <= 0 || port > 65535) return;
    QVariantMap h = s()["host"].toMap();
    h["port"] = port;
    s()["host"] = h;
    savePrefs();
    touch();
}

void Desk::hostDecide(bool allow) {
    QProcess* p = m_procs.value(QStringLiteral("host"));
    QVariantMap h = s()["host"].toMap();
    if (p && p->state() == QProcess::Running &&
        !h["pendingDevice"].toString().isEmpty()) {
        p->write(allow ? "y\n" : "n\n");
    }
    h["pendingDevice"] = QString();
    h["pendingFp"] = QString();
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

// Open the file itself with the default handler (image viewer).
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
