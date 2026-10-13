#include "desk.h"
#include "parser.h"

#include <QClipboard>
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
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr int CoalesceMs = 70;

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
    s["cliVersion"] = QStringLiteral("v0.1.0 avif:system-dlopen");
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
                s()["cliVersion"] =
                    parser::parseVersion(out.section(QLatin1Char('\n'), 0, 0));
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

    QProcess*& slot = m_procs[key];
    if (slot) {
        slot->disconnect(this);
        slot->deleteLater();
    }
    slot = new QProcess(this);
    slot->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    for (const QString& kv : env) {
        const int eq = kv.indexOf(QLatin1Char('='));
        e.insert(kv.left(eq), kv.mid(eq + 1));
    }
    slot->setProcessEnvironment(e);
    connect(slot, &QProcess::errorOccurred, this, [this, key](QProcess::ProcessError) {
        if (key == QLatin1String("job")) finishJob(-1);
    });
    slot->start(cli, args);
    if (!slot->waitForStarted(3000)) {
        slot->deleteLater();
        slot = nullptr;
        return nullptr;
    }
    return slot;
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

void Desk::savePrefs() const {
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

    for (const QString& raw : paths) {
        QString path = raw;
        if (path.startsWith(QStringLiteral("file://")))
            path = QUrl(path).toLocalFile();
        path = QDir::cleanPath(path);
        QFileInfo info(path);
        if (!info.exists()) continue;

        if (info.isDir()) {
            QDirIterator it(path, imageExtensions(), QDir::Files,
                            QDirIterator::Subdirectories);
            QStringList files;
            while (it.hasNext()) files.append(it.next());
            files.sort();
            if (files.isEmpty()) {
                setNotice(QStringLiteral("文件夹里没有图片"));
                continue;
            }
            folderCount = files.size();
            cover = QUrl::fromLocalFile(files.first()).toString();
            inputs = QVariantList{inputEntry(path, true)};
        } else if (isImageFile(path)) {
            bool dup = false;
            for (const QVariant& v : inputs)
                if (v.toMap().value("path") == path) { dup = true; break; }
            if (dup) continue;
            if (inputs.size() == 1 && inputs.first().toMap().value("dir").toBool())
                inputs.clear();
            inputs.append(inputEntry(path, false));
            folderCount = 0;
            cover.clear();
        }
    }
    s()["inputs"] = inputs;
    s()["folderCount"] = folderCount;
    s()["cover"] = cover;
    s()["job"] = QVariantMap(defaultSnap().value("job").toMap());
    touch();
}

void Desk::clearInputs() {
    s()["inputs"] = QVariantList{};
    s()["folderCount"] = 0;
    s()["cover"] = QString();
    s()["job"] = QVariantMap(defaultSnap().value("job").toMap());
    touch();
}

void Desk::removeInput(const QString& path) {
    QVariantList inputs;
    for (const QVariant& v : s()["inputs"].toList())
        if (v.toMap().value("path") != path) inputs.append(v);
    s()["inputs"] = inputs;
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
    const QVariantList inputs = s()["inputs"].toList();
    if (inputs.isEmpty() || s()["model"].toString().isEmpty()) return;

    QStringList args{QStringLiteral("-i"), inputs.first().toMap().value("path").toString(),
                     QStringLiteral("-m"), s()["model"].toString(),
                     QStringLiteral("--output-quality"), QString::number(s()["quality"].toInt())};
    const QString fmt = s()["format"].toString();
    if (!fmt.isEmpty()) args << QStringLiteral("--output-format") << fmt;
    if (s()["autoTile"].toBool()) args << QStringLiteral("--auto-tile");
    else args << QStringLiteral("--tile-size") << QString::number(s()["tileSize"].toInt());
    if (s()["force"].toBool()) args << QStringLiteral("-f");
    const bool dir = inputs.first().toMap().value("dir").toBool();
    // --auto-tile is folder-only: on the CLI it gates out the warm-daemon
    // delegation (the optimizer must benchmark the inline engine; ServeRequest
    // cannot carry it). Without it a single image delegates to the daemon —
    // which auto-spawns one and applies the engine's recommended tile size.
    // A pinned tile size rides along in the request either way.
    if (dir && s()["autoTile"].toBool()) args << QStringLiteral("--auto-tile");
    if (!s()["autoTile"].toBool())
        args << QStringLiteral("--tile-size") << QString::number(s()["tileSize"].toInt());
    if (dir) {
        if (s()["gpu"].toBool()) args << QStringLiteral("--gpu");
        else args << QStringLiteral("-j") << QString::number(s()["jobs"].toInt());
    }

    QVariantMap job = defaultSnap().value("job").toMap();
    job["phase"] = QStringLiteral("running");
    job["total"] = dir ? inputs.size() : 1;
    s()["job"] = job;
    touch();

    QProcess* p = spawn(QStringLiteral("job"), args,
                        {QStringLiteral("UPIX_PROGRESS=1")});
    if (!p) {
        finishJob(-1);
        return;
    }
    connect(p, &QProcess::readyRead, this, [this, p] {
        const QStringList lines =
            QString::fromUtf8(p->readAll()).split(QLatin1Char('\n'));
        QVariantMap job = s()["job"].toMap();
        bool dirty = false;
        for (const QString& line : lines) {
            const QVariantMap prog = parser::parseProgress(line);
            if (prog.value("percent").toInt() >= 0) {
                job["percent"] = prog.value("percent");
                job["tilesDone"] = prog.value("done");
                job["tilesTotal"] = prog.value("total");
                job["etaSec"] = prog.value("eta");
                dirty = true;
                continue;
            }
            const QString saved = parser::parseSaved(line);
            if (!saved.isEmpty()) {
                QVariantList outs = job["outputs"].toList();
                outs.append(QUrl::fromLocalFile(saved).toString());
                job["outputs"] = outs;
                dirty = true;
                continue;
            }
            const QString current = parser::parseCurrentFile(line);
            if (!current.isEmpty()) {
                job["current"] = current;
                dirty = true;
            }
            const QString note = parser::parseDoneNote(line);
            if (!note.isEmpty()) {
                job["message"] = note;
                dirty = true;
            }
        }
        if (dirty) {
            s()["job"] = job;
            touch();
        }
    });
    connect(p, &QProcess::finished, this,
            [this, p](int code, QProcess::ExitStatus st) { finishJob(code == 0 && st == QProcess::NormalExit ? 0 : 1); });
}

void Desk::finishJob(int code) {
    QProcess* p = m_procs.take(QStringLiteral("job"));
    if (p) {
        p->disconnect(this);
        p->deleteLater();
    }
    QVariantMap job = s()["job"].toMap();
    const bool running = job.value("phase").toString() == QLatin1String("running");
    if (!running) return;

    if (code == 0) {
        job["phase"] = QStringLiteral("done");
        job["percent"] = 100;
        if (job.value("message").toString().isEmpty())
            job["message"] = QStringLiteral("完成");
    } else {
        job["phase"] = QStringLiteral("error");
        job["message"] = p ? QString::fromUtf8(p->readAll()).trimmed().section(QLatin1Char('\n'), -1)
                           : QStringLiteral("无法启动 upix-cli");
        if (job.value("message").toString().isEmpty())
            job["message"] = QStringLiteral("处理失败");
        setNotice(job.value("message").toString());
    }
    s()["job"] = job;
    touch();
    // The job may have spawned the warm engine (single-image delegation);
    // the row would otherwise keep a stale 未运行 until the next restart.
    refreshDaemonStatus();
}

void Desk::cancel() {
    QProcess* p = m_procs.value(QStringLiteral("job"));
    if (!p || p->state() != QProcess::Running) return;
    p->terminate();
    if (!p->waitForFinished(50)) {
        // async kill after grace period; never blocks the GUI thread
        QPointer<QProcess> guard(p);
        QTimer::singleShot(2000, this, [guard] {
            if (guard && guard->state() == QProcess::Running) guard->kill();
        });
    }
}

// ---- models ------------------------------------------------------------

void Desk::refreshModels() {
    QProcess* p = spawn(QStringLiteral("models"), {QStringLiteral("model"), QStringLiteral("list")});
    if (!p) return;
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        if (code != 0) return;
        const QStringList lines = QString::fromUtf8(p->readAllStandardOutput())
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QVariantMap m = parser::parseModelList(lines);
        s()["modelsDir"] = m.value("modelsDir");
        s()["installed"] = m.value("installed");
        touch();

        QProcess* r = spawn(QStringLiteral("remote"),
                            {QStringLiteral("model"), QStringLiteral("list-remote")});
        if (!r) return;
        connect(r, &QProcess::finished, this, [this, r](int code, QProcess::ExitStatus) {
            if (code != 0) {
                s()["remoteError"] = QStringLiteral("远端列表获取失败");
                touch();
                return;
            }
            s()["remoteError"] = QString();
            s()["remote"] = parser::parseRemoteList(
                QString::fromUtf8(r->readAllStandardOutput())
                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts));
            touch();
        });
    });
}

void Desk::importModels(const QStringList& paths) {
    if (paths.isEmpty()) return;
    QProcess* p = spawn(QStringLiteral("import"),
                        QStringList{QStringLiteral("model"), QStringLiteral("import")} + paths);
    if (!p) return;
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(p->readAll());
        if (code == 0) {
            setNotice(QStringLiteral("导入完成"));
            refreshModels();
        } else {
            setNotice(out.section(QLatin1Char('\n'), -2, -2).trimmed());
        }
    });
}

void Desk::download(const QString& name) {
    if (s()["downloading"].toString() == name) return;
    QProcess* p = spawn(QStringLiteral("download"),
                        {QStringLiteral("model"), QStringLiteral("download"), name});
    if (!p) {
        setNotice(QStringLiteral("未找到 upix-cli"));
        return;
    }
    s()["downloading"] = name;
    s()["downloadPct"] = 0;
    touch();
    connect(p, &QProcess::readyRead, this, [this, p, name] {
        const int pct = parser::parseDownloadPercent(QString::fromUtf8(p->readAll()));
        if (pct >= 0 && pct != s()["downloadPct"].toInt()) {
            s()["downloadPct"] = pct;
            touch();
        }
    });
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

// ---- daemon ------------------------------------------------------------

void Desk::refreshDaemonStatus() {
    QProcess* p = spawn(QStringLiteral("dstatus"), {QStringLiteral("daemon"), QStringLiteral("status")});
    if (!p) return;
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        const QVariantMap d = parser::parseDaemonStatus(
            QString::fromUtf8(p->readAllStandardOutput()).trimmed());
        s()["daemon"] = d;
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

void Desk::hostStart() {
    if (!s()["cliFound"].toBool()) return;
    hostReset();
    QProcess* p = spawn(QStringLiteral("host"),
                        {QStringLiteral("host"), QStringLiteral("start"),
                         QStringLiteral("-p"), QString::number(s()["host"].toMap().value("port").toInt())});
    if (!p) return;
    connect(p, &QProcess::readyRead, this, [this, p] {
        const QStringList lines = QString::fromUtf8(p->readAll()).split(QLatin1Char('\n'));
        QVariantMap h = s()["host"].toMap();
        bool dirty = false;
        QString device, fp;
        for (const QString& line : lines) {
            const parser::HostCard card = parser::parseHostCard(
                QStringList{QStringLiteral("Connection Information:"), line});
            if (!card.qrArt.isEmpty()) {
                h["qr"] = card.qrArt;
                dirty = true;
                continue;
            }
            if (card.valid) {
                h["fingerprint"] = card.fingerprint;
                if (card.port > 0) h["port"] = card.port;
                QVariantList ips = h["ips"].toList();
                for (const QString& ip : card.ips)
                    if (!ips.contains(ip)) ips.append(ip);
                h["ips"] = ips;
                h["running"] = true;
                dirty = true;
                continue;
            }
            if (parser::parsePairing(line, &device, &fp)) {
                if (!device.isEmpty()) h["pendingDevice"] = device;
                if (!fp.isEmpty()) h["pendingFp"] = fp;
                dirty = true;
                continue;
            }
            const int pct = parser::parseHostTaskPercent(line);
            if (pct >= 0) {
                h["taskPercent"] = pct;
                dirty = true;
            }
            if (line.contains(QStringLiteral("[INFO]"))) {
                QVariantList log = h["log"].toList();
                log.append(line.section(QStringLiteral("] "), 2));
                while (log.size() > 50) log.removeFirst();
                h["log"] = log;
                dirty = true;
            }
        }
        if (dirty) {
            s()["host"] = h;
            touch();
        }
    });
    connect(p, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        hostReset();
        refreshDaemonStatus();
    });
}

void Desk::hostStop() {
    QProcess* p = m_procs.value(QStringLiteral("host"));
    if (p && p->state() == QProcess::Running) p->terminate();
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
