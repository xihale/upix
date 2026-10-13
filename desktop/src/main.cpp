// Bootstrap + headless tooling.
//
// Startup-order rules (all three were bugs in the previous build):
//  1. Read argv from the raw C array — QCoreApplication::arguments() before
//     an application exists aborts on some setups.
//  2. QGuiApplication::setHighDpiScaleFactorRoundingPolicy() BEFORE creating
//     the application object.
//  3. This binary is the GUI, not the CLI. Any CLI-style invocation
//     (`upix model list`, unknown positional…) prints usage to stderr and
//     exits 2 WITHOUT constructing a QGuiApplication or any window. The old
//     build booted the full GUI for those, spawned copies of itself as
//     "the CLI", and the pile of GPU windows crashing took kwin down —
//     the whole-desktop freeze.
#include "desk.h"
#include "icons.h"
#include "qrprovider.h"
#include "theme.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QApplication>
#include <QImage>
#include <QImageReader>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>
#include <QCommandLineParser>

namespace {

enum class Mode { Gui, Shot, Anim, Sweep, Smoke };

struct Options {
    Mode mode = Mode::Gui;
    QStringList states;      // --shot a,b,c
    QString out = "shots-qt";
};

// argv parsed from the raw array; no QCoreApplication::arguments() here.
Options parseArgs(int argc, char** argv, bool* reject) {
    Options o;
    *reject = false;
    const QSet<QString> cliSubcommands = {
        QStringLiteral("model"), QStringLiteral("host"), QStringLiteral("daemon"),
        QStringLiteral("process"), QStringLiteral("complete"),
        QStringLiteral("completions")};
    bool nextIsOut = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (nextIsOut) {
            o.out = a;
            nextIsOut = false;
            continue;
        }
        if (a == QLatin1String("--shot")) {
            o.mode = Mode::Shot;
        } else if (a == QLatin1String("--anim")) {
            o.mode = Mode::Anim;
        } else if (a == QLatin1String("--sweep")) {
            o.mode = Mode::Sweep;
        } else if (a == QLatin1String("--smoke")) {
            o.mode = Mode::Smoke;
        } else if (a == QLatin1String("--out") && !a.isEmpty()) {
            nextIsOut = true;
        } else if (a.startsWith(QLatin1String("--shot="))) {
            o.mode = Mode::Shot;
            o.states = a.mid(7).split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else if (a.startsWith(QLatin1String("--out="))) {
            o.out = a.mid(6);
        } else if (a.startsWith(QLatin1Char('-'))) {
            // Qt/platform flags pass through untouched.
        } else if (o.mode == Mode::Shot && o.states.isEmpty()) {
            o.states = a.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else {
            // Positional argument: this is a CLI-style invocation. Refuse
            // headlessly — never construct windows for it.
            qWarning("upix-desktop is the GUI; the CLI is `upix-cli`.");
            qWarning("Refused headless (no window created): %s", argv[i]);
            *reject = true;
            return o;
        }
    }
    if (o.mode == Mode::Shot && o.states.isEmpty())
        o.states = QStringList{QStringLiteral("empty")};
    return o;
}

void pump(QGuiApplication& app, int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

bool grab(QQuickWindow* win, const QString& path) {
    const QImage img = win->grabWindow();
    if (img.isNull()) {
        qWarning("grab failed: %s", qUtf8Printable(path));
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    return img.save(path);
}

int runHeadlessShots(QGuiApplication& app, QQmlApplicationEngine& engine,
                     const Options& o) {
    if (engine.rootObjects().isEmpty()) return 1;
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!win || !Desk::instance()) return 1;

    QDir().mkpath(o.out);
    int rc = 0;
    if (o.mode == Mode::Shot) {
        for (const QString& state : o.states) {
            Desk::instance()->applyFakeSnapshot(Desk::fakeSnapshot(state));
            // page state lives QML-side; point the shell at the right page
            if (state == QLatin1String("models"))
                win->setProperty("view", QStringLiteral("models"));
            else if (state == QLatin1String("host") || state == QLatin1String("pairing")
                     || state == QLatin1String("host-idle") || state == QLatin1String("host-blank"))
                win->setProperty("view", QStringLiteral("host"));
            else
                win->setProperty("view", QStringLiteral("stage"));
            // Full boot stagger (420+480+480 ms) must finish before a shot
            // claims to show the settled UI.
            pump(app, 1600);
            if (!grab(win, o.out + QLatin1Char('/') + state + QStringLiteral(".png")))
                rc = 1;
        }
        return rc;
    }

    if (o.mode == Mode::Anim) {
        Desk::instance()->applyFakeSnapshot(Desk::fakeSnapshot(QStringLiteral("empty")));
        pump(app, 30); // let the boot animation start from frame 0
        grab(win, o.out + QStringLiteral("/empty-entry-start.png"));
        pump(app, 210);
        grab(win, o.out + QStringLiteral("/empty-entry-mid.png"));
        pump(app, 560);
        grab(win, o.out + QStringLiteral("/empty-entry-end.png"));
        return 0;
    }

    // Sweep: compare divider animates right after the done snapshot lands.
    Desk::instance()->applyFakeSnapshot(Desk::fakeSnapshot(QStringLiteral("done")));
    pump(app, 20);
    grab(win, o.out + QStringLiteral("/sweep-start.png"));
    pump(app, 360);
    grab(win, o.out + QStringLiteral("/sweep-mid.png"));
    pump(app, 400);
    grab(win, o.out + QStringLiteral("/sweep-end.png"));
    return 0;
}

// Real end-to-end run through the discovered CLI: tiny generated PNG in,
// upscaled file out. Exit 0 on success.
int runSmoke(QGuiApplication& app) {
    const QString cli = Desk::instance() ? Desk::instance()->findCli() : QString();
    fprintf(stderr, "[smoke] cli=%s\n", cli.isEmpty() ? "<none>" : qUtf8Printable(cli));
    if (cli.isEmpty()) {
        fprintf(stderr, "smoke: no upix-cli found\n");
        return 1;
    }
    const QString dir = QDir::tempPath() + QStringLiteral("/upix-smoke");
    QDir().mkpath(dir);
    QImage in(64, 64, QImage::Format_RGB32);
    in.fill(0xFF336699);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) in.setPixel(x, y, qRgb(x * 4, y * 4, 128));
    const QString inPath = dir + QStringLiteral("/in.png");
    const QString outPath = dir + QStringLiteral("/out.png");
    if (!in.save(inPath)) {
        fprintf(stderr, "smoke: failed to save %s\n", qUtf8Printable(inPath));
        return 1;
    }

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(cli, {inPath, outPath});
    if (!p.waitForStarted(5000)) {
        fprintf(stderr, "smoke: start failed: %s\n", qUtf8Printable(p.errorString()));
        return 1;
    }
    QElapsedTimer t;
    t.start();
    while (p.state() == QProcess::Running && t.elapsed() < 120000) {
        app.processEvents(QEventLoop::AllEvents, 50);
    }
    if (p.state() == QProcess::Running) {
        p.kill();
        qWarning("smoke: timeout");
        return 1;
    }
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 &&
                    QFileInfo::exists(outPath);
    fprintf(stderr, "[smoke] state=%d exit=%d exists=%d\n%s\n",
            int(p.exitStatus()), p.exitCode(), int(QFileInfo::exists(outPath)),
            qUtf8Printable(QString::fromUtf8(p.readAll())));
    fprintf(stderr, "smoke: %s\n", ok ? "ok" : "failed");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    // 1. raw argv before any QCoreApplication exists
    bool reject = false;
    const Options o = parseArgs(argc, argv, &reject);
    if (reject) return 2;

    // 2. DPI policy before the application object
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    if (o.mode == Mode::Shot || o.mode == Mode::Anim || o.mode == Mode::Sweep) {
        // fixture data + software rendering; no real display needed
        qputenv("UPIX_FAKE", "1");
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");
    } else if (o.mode == Mode::Smoke) {
        // no rendering at all — only spawns the CLI
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("upix"));
    QApplication::setApplicationName(QStringLiteral("desktop"));
    // Stock controls, deterministic across desktops — never the system QQC2
    // style (mixing styles with the Basic imports crashed Breeze at startup).
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    // Upscaler outputs routinely exceed Qt's 256 MB decode ceiling (a ×4 of a
    // 2560×1680 AVIF is ~275 MB decompressed). Files here come from the user's
    // own CLI runs — drop the ceiling instead of failing to display results.
    QImageReader::setAllocationLimit(0);

    QQmlApplicationEngine engine;
    // Context properties: visible unqualified from every QML file in this
    // engine, independent of module qmldir/typeinfo generation (the build-dir
    // qmldir shadows programmatic registrations and lists no C++ types).
    auto* theme = new Theme(&app);
    auto* desk = new Desk(&app);
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("Desk"), desk);
    // Same tokens drive the QPalette so stock controls (Button, ComboBox,
    // Slider, Switch…) match the chrome and flip with UPIX_THEME.
    {
        QPalette pal = app.palette();
        pal.setColor(QPalette::Window, theme->bg());
        pal.setColor(QPalette::WindowText, theme->onSurface());
        pal.setColor(QPalette::Base, theme->surface());
        pal.setColor(QPalette::AlternateBase, theme->container());
        pal.setColor(QPalette::Text, theme->onSurface());
        pal.setColor(QPalette::PlaceholderText, theme->muted());
        pal.setColor(QPalette::Button, theme->high());
        pal.setColor(QPalette::ButtonText, theme->onSurface());
        pal.setColor(QPalette::Highlight, theme->primary());
        pal.setColor(QPalette::HighlightedText, theme->onPrimary());
        pal.setColor(QPalette::ToolTipBase, theme->highest());
        pal.setColor(QPalette::ToolTipText, theme->onSurface());
        pal.setColor(QPalette::Link, theme->primary());
        pal.setColor(QPalette::Light, theme->highest());
        pal.setColor(QPalette::Midlight, theme->high());
        pal.setColor(QPalette::Mid, theme->outlineV());
        pal.setColor(QPalette::Dark, theme->bg());
        const QColor dis = theme->muted();
        pal.setBrush(QPalette::Disabled, QPalette::Text, dis);
        pal.setBrush(QPalette::Disabled, QPalette::ButtonText, dis);
        pal.setBrush(QPalette::Disabled, QPalette::WindowText, dis);
        app.setPalette(pal);
    }
    engine.addImageProvider(QStringLiteral("qr"), new QrProvider);
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;

    switch (o.mode) {
    case Mode::Shot:
    case Mode::Anim:
    case Mode::Sweep:
        return runHeadlessShots(app, engine, o);
    case Mode::Smoke:
        return runSmoke(app);
    case Mode::Gui:
        break;
    }
    return app.exec();
}
