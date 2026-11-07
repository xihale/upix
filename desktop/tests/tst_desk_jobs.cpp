#include "desk.h"
#include "machine.h"

#include "core/distributed/protocol.hpp"
#include "core/model_families.hpp"
#include "utils/unix_socket.hpp"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// A real AF_UNIX server speaking the serve protocol well enough for the
// desktop delegation: reads one request frame per connection, then replies
// according to `mode`. Frames are built with the production Protocol code —
// the assertions pin the real wire, not a mock echo.
class FakeServeDaemon {
public:
    enum Mode { Saved, Skipped, LegacyFrame };

    explicit FakeServeDaemon(const QString& path) {
        server_ = std::make_unique<upix::UnixServer>();
        if (!server_->listen(path.toStdString()).has_value())
            qFatal("fake daemon listen failed");
        thread_ = std::thread([this] {
            while (true) {
                auto conn = server_->accept();
                if (!conn) return;  // stop() woke the poll
                serve(**conn);
            }
        });
    }
    ~FakeServeDaemon() {
        server_->stop();
        if (thread_.joinable()) thread_.join();
    }

    Mode mode = Saved;
    QString savedPath;
    QString lastModel;

private:
    void serve(upix::UnixSocket& sock) {
        std::vector<uint8_t> hdr(upix::Protocol::HEADER_SIZE);
        if (!sock.receive_all(hdr.data(), hdr.size()).has_value()) return;
        const auto h = upix::Protocol::parse_header(hdr);
        if (!h || h->type != upix::MessageType::MSG_SERVE_REQUEST) return;
        std::vector<uint8_t> body(h->payload_length);
        if (!sock.receive_all(body.data(), body.size()).has_value()) return;
        upix::ServeRequest req{};
        std::memcpy(&req, body.data(), std::min(body.size(), sizeof(req)));
        lastModel = QByteArray(req.model_name).trimmed();

        const auto send = [&](const void* payload, size_t len) {
            upix::MessageHeader out{};
            out.magic = upix::Protocol::MAGIC;
            out.type = upix::MessageType::MSG_SERVE_RESPONSE;
            out.payload_length = static_cast<uint32_t>(len);
            const auto frame = upix::Protocol::serialize(out, payload);
            (void)sock.send_all(frame.data(), frame.size());
        };

        if (mode == Skipped) {
            upix::ServeResponse r{};
            r.ok = 1;
            r.skipped = 1;
            send(&r, sizeof(r));
            return;
        }
        // One progress frame first: pins that live tile events flow.
        upix::ServeProgress p{2, 4, 0};
        upix::MessageHeader ph{};
        ph.magic = upix::Protocol::MAGIC;
        ph.type = upix::MessageType::MSG_SERVE_PROGRESS;
        ph.payload_length = sizeof(p);
        const auto pframe = upix::Protocol::serialize(ph, &p);
        (void)sock.send_all(pframe.data(), pframe.size());

        upix::ServeResponse r{};
        r.ok = 1;
        r.width = 320;
        r.height = 240;
        const std::string out = savedPath.toStdString();
        r.out_path_length = static_cast<uint32_t>(out.size());
        if (mode == LegacyFrame) {
            // A same-repo older daemon: the response struct without the
            // 8-byte skipped tail, path bytes right after (SPEC §4).
            send(&r, sizeof(r) - 8);
            (void)sock.send_all(out.data(), out.size());
            return;
        }
        std::string payload(reinterpret_cast<const char*>(&r), sizeof(r));
        payload += out;
        send(payload.data(), payload.size());
    }

    std::unique_ptr<upix::UnixServer> server_;
    std::thread thread_;
};

} // namespace

namespace {

class TstDesktopJobs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void machineParsesEventLines();
    void daemonLabelCoversAllStatusShapes();
    void modelFamiliesGoldenPinsShareSemantics();
    void servesFileInputsThroughWarmDaemon();
    void processesEveryInputOnceWithSnapshottedSettings();
    void cancellationStopsQueuedInputs();
    void failureKeepsDiagnosticOutput();
    void liveHostOutputKeepsCardAndPairing();
    void runSurvivesInputEdits();
    void secondStartDuringRunIsIgnored();
    void failedToStartSurfacesError();
    void folderCountAndCoverFollowInputRemoval();
    void unsupportedDropIsReported();
    void cliVersionGateFlagsOldCliAndPassesNew();

private:
    QString addImage(const QString& name);
    void prepareDesk(Desk& desk, const QStringList& paths, bool autoTile = false);
    QTemporaryDir m_dir;
    QString m_cli;
    QString m_log;
};

void TstDesktopJobs::initTestCase() {
    QVERIFY(m_dir.isValid());
    m_cli = m_dir.filePath(QStringLiteral("fake-upix"));
    m_log = m_dir.filePath(QStringLiteral("invocations.log"));
    QFile script(m_cli);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    // The fake CLI speaks the machine protocol: one JSON object per line on
    // stdout. Deliberately split across writes (sleep between printf) so the
    // line-assembly path is exercised, not just whole-buffer reads.
    script.write(R"(#!/bin/sh
case "$1" in
  daemon)
    [ "$2" = status ] && printf '{"running":true,"pid":4321,"socket":"/home/u/.cache/upix/serve.sock","socketMissing":false}\n'
    exit 0 ;;
  --version|-V)
    printf 'v%s avif:system-dlopen\n' "${UPIX_TEST_CLI_VERSION:-0.2.0}"
    exit 0 ;;
esac
printf '%s\n' "$*" >> "$UPIX_TEST_LOG"
case "$UPIX_TEST_MODE" in
  host)
    printf '{"t":"card","ips":["192.0.'
    sleep 0.03
    printf '2.4"],"port":43210,"fingerprint":"ABCDEF0123456789","qr":"{\\"v\\":2,\\"ip\\":\\"192.0.2.4\\",\\"port\\":43210,\\"fp\\":\\"ABCDEF0123456789\\",\\"pk\\":\\"dGVzdA==\\"}"}\n'
    printf '[2026-10-15 10:00:00.123] [Host] [INFO] Coordinator running.\n' >&2
    sleep 0.03
    printf '{"t":"pairing","device":"Pixel 9 Pro","fingerprint":"FEDCBA9876543210"}\n'
    sleep 5
    ;;
  cancel)
    sleep 5
    ;;
  failure)
    printf '[2026-10-15 10:00:00.123] [Engine] [ERROR] diagnostic stderr detail\n' >&2
    exit 9
    ;;
  *)
    input=
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "-i" ]; then shift; input=$1; fi
      shift
    done
    printf '{"t":"progress","pct":42,"done":5,'
    sleep 0.03
    printf '"total":12,"eta":3}\n'
    printf '{"t":"saved","path":"%s.out"}\n' "$input"
    sleep 0.08
    ;;
esac
)" );
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("UPIX_FAKE", "1");
    qputenv("UPIX_CLI", m_cli.toUtf8());
    qputenv("UPIX_TEST_LOG", m_log.toUtf8());
    qputenv("UPIX_TEST_MODE", "success");
}

void TstDesktopJobs::init() {
    QFile::remove(m_log);
}

void TstDesktopJobs::cleanup() {
    // A test that bails halfway (QVERIFY) must not leave the next one with the
    // broken CLI path or a non-default mode.
    qputenv("UPIX_CLI", m_cli.toUtf8());
    qputenv("UPIX_TEST_MODE", "success");
    qputenv("UPIX_FAKE", "1");
    qunsetenv("UPIX_TEST_CLI_VERSION");
    qunsetenv("UPIX_SERVE_SOCKET");
}

QString TstDesktopJobs::addImage(const QString& name) {
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write("image");
    return path;
}

void TstDesktopJobs::prepareDesk(Desk& desk, const QStringList& paths, bool autoTile) {
    QVariantMap snap = Desk::fakeSnapshot(QStringLiteral("picked"));
    snap["inputs"] = QVariantList{};
    snap["folderCount"] = 0;
    snap["cover"] = QString();
    snap["cliFound"] = true;
    snap["model"] = QStringLiteral("test-model");
    snap["autoTile"] = autoTile;
    snap["tileSize"] = 64;
    snap["gpu"] = false;
    snap["jobs"] = 3;
    desk.applyFakeSnapshot(snap);
    desk.addPaths(paths);
}

void TstDesktopJobs::machineParsesEventLines() {
    // Events parse; log lines and one-shot documents are not events.
    QVariantMap e = machine::parseEvent(
        QStringLiteral(R"({"t":"progress","pct":42,"done":5,"total":12,"eta":3})"));
    QCOMPARE(e.value("t").toString(), QStringLiteral("progress"));
    QCOMPARE(e.value("pct").toInt(), 42);
    QCOMPARE(e.value("done").toInt(), 5);
    QCOMPARE(e.value("total").toInt(), 12);
    QCOMPARE(e.value("eta").toInt(), 3);

    e = machine::parseEvent(QStringLiteral(R"({"t":"card","ips":["192.0.2.4","10.0.0.2"],"port":9527,"fingerprint":"EC61F2B66D4C25C4","qr":"{\"v\":2}"})"));
    QCOMPARE(e.value("t").toString(), QStringLiteral("card"));
    QCOMPARE(e.value("ips").toList().size(), 2);
    QCOMPARE(e.value("port").toInt(), 9527);
    QVERIFY(e.value("qr").toString().contains(QStringLiteral("\"v\":2")));

    QVERIFY(machine::parseEvent(QStringLiteral("[2026-10-15] [Engine] [ERROR] boom")).isEmpty());
    QVERIFY(machine::parseEvent(QStringLiteral(R"({"running":false,"stale":true})")).isEmpty());
    QVERIFY(machine::parseEvent(QStringLiteral("not json at all")).isEmpty());
    QVERIFY(machine::parseEvent(QStringLiteral("")).isEmpty());
}

void TstDesktopJobs::daemonLabelCoversAllStatusShapes() {
    QCOMPARE(machine::daemonLabel(QVariantMap{{"running", true}, {"pid", 4321},
                                              {"socket", QStringLiteral("/home/u/.cache/upix/serve.sock")},
                                              {"socketMissing", false}}),
             QStringLiteral("running (pid 4321, socket /home/u/.cache/upix/serve.sock)"));
    QCOMPARE(machine::daemonLabel(QVariantMap{{"running", true}, {"pid", 7},
                                              {"socket", QStringLiteral("/x.sock")},
                                              {"socketMissing", true}}),
             QStringLiteral("running (pid 7, socket /x.sock) — socket file missing!"));
    QCOMPARE(machine::daemonLabel(QVariantMap{{"running", false}, {"stale", true}}),
             QStringLiteral("not running (stale pid file)"));
    QCOMPARE(machine::daemonLabel(QVariantMap{{"running", false}, {"stale", false}}),
             QStringLiteral("not running"));
}

void TstDesktopJobs::modelFamiliesGoldenPinsShareSemantics() {
    // share/core/model_families.hpp is the single source (SPEC §1 S3); pin
    // the semantics the renderer relies on: known families, unknown stems
    // split on -xN, lone members and single-member stems stay ungrouped.
    const auto g = upix::groups({
        std::string("Real-ESRGANv3"),
        std::string("Real-ESRGANv3-AnimeVideo-v3-x2"),
        std::string("NomosUni-SPAN-x2"),
        std::string("NomosUni-SPAN-x4"),
        std::string("BSRGAN-x4"),
    });
    QCOMPARE(g.size(), 3UL);
    const auto find = [&g](const std::string& label) -> const upix::FamilyGroup* {
        for (const auto& grp : g)
            if (grp.label == label) return &grp;
        return nullptr;
    };
    const upix::FamilyGroup* family = find("Real-ESRGAN v3");
    QVERIFY(family);
    QCOMPARE(family->items.size(), 2UL);
    QCOMPARE(QString::fromStdString(family->items[1].short_name),
             QStringLiteral("AnimeVideo-v3-x2"));
    const upix::FamilyGroup* stem = find("NomosUni-SPAN");
    QVERIFY(stem);
    QCOMPARE(stem->items.size(), 2UL);
    QCOMPARE(QString::fromStdString(stem->items[1].short_name),
             QStringLiteral("x4"));
    const upix::FamilyGroup* lone = nullptr;
    for (const auto& grp : g)
        if (grp.label.empty()) lone = &grp;
    QVERIFY(lone);
    QCOMPARE(lone->items.size(), 1UL);
    QCOMPARE(QString::fromStdString(lone->items[0].name),
             QStringLiteral("BSRGAN-x4"));

    // A lone KNOWN member gains nothing from a family header.
    const auto single = upix::groups({std::string("Real-ESRGANv3")});
    QCOMPARE(single.size(), 1UL);
    QVERIFY(single[0].label.empty());
    QCOMPARE(QString::fromStdString(single[0].items[0].name),
             QStringLiteral("Real-ESRGANv3"));
}

void TstDesktopJobs::servesFileInputsThroughWarmDaemon() {
    // Stage 1 (SPEC §3): a single file with a pinned tile size on GPU rides
    // the serve protocol in-process — no CLI spawn. Three wire shapes: a
    // normal save, an SR-marker skip, and a same-repo older daemon's legacy
    // frame (SPEC §4 tail tolerance).
    for (const FakeServeDaemon::Mode mode :
         {FakeServeDaemon::Saved, FakeServeDaemon::Skipped,
          FakeServeDaemon::LegacyFrame}) {
        const QString sock = m_dir.filePath(QStringLiteral("serve-%1.sock").arg(int(mode)));
        FakeServeDaemon daemon(sock);
        daemon.mode = mode;
        const QString image = addImage(QStringLiteral("srv-%1.png").arg(int(mode)));
        daemon.savedPath = image + QStringLiteral(".out");
        qputenv("UPIX_SERVE_SOCKET", sock.toUtf8());

        Desk desk;
        QVariantMap snap = Desk::fakeSnapshot(QStringLiteral("picked"));
        snap["inputs"] = QVariantList{};
        snap["folderCount"] = 0;
        snap["cover"] = QString();
        snap["cliFound"] = true;
        snap["model"] = QStringLiteral("test-model");
        snap["autoTile"] = false;
        snap["tileSize"] = 128;
        snap["gpu"] = true;
        snap["force"] = false;
        desk.applyFakeSnapshot(snap);
        desk.addPaths({image});
        desk.start();

        QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                                  QStringLiteral("done"), 5000);
        const QVariantMap job = desk.snap().value("job").toMap();
        QCOMPARE(job.value("percent").toInt(), 100);
        QCOMPARE(job.value("outputs").toList().size(),
                 mode == FakeServeDaemon::Skipped ? 0 : 1);
        if (mode == FakeServeDaemon::Saved)
            QCOMPARE(job.value("outputs").toList().at(0).toString(),
                     QUrl::fromLocalFile(image + QStringLiteral(".out")).toString());
        if (mode == FakeServeDaemon::Skipped)
            QVERIFY(job.value("message").toString().contains(QStringLiteral("已跳过")));
        QCOMPARE(daemon.lastModel, QByteArrayLiteral("test-model"));
        qunsetenv("UPIX_SERVE_SOCKET");
    }
}

void TstDesktopJobs::processesEveryInputOnceWithSnapshottedSettings() {
    qputenv("UPIX_TEST_MODE", "success");
    Desk desk;
    const QString first = addImage(QStringLiteral("first.png"));
    const QString second = addImage(QStringLiteral("second.png"));
    prepareDesk(desk, {first, second}, false);
    desk.start();

    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("percent").toInt(), 42, 2000);
    QCOMPARE(desk.snap().value("job").toMap().value("tilesDone").toInt(), 5);
    QCOMPARE(desk.snap().value("job").toMap().value("tilesTotal").toInt(), 12);
    QCOMPARE(desk.snap().value("job").toMap().value("etaSec").toInt(), 3);
    QCOMPARE(desk.snap().value("job").toMap().value("index").toInt(), 1);
    QCOMPARE(desk.snap().value("job").toMap().value("total").toInt(), 2);
    QVariantMap changed = desk.snap();
    changed["model"] = QStringLiteral("changed-model");
    changed["tileSize"] = 512;
    desk.applyFakeSnapshot(changed);
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("done"), 5000);

    const QVariantMap job = desk.snap().value("job").toMap();
    QCOMPARE(job.value("index").toInt(), 2);
    QCOMPARE(job.value("total").toInt(), 2);
    QCOMPARE(job.value("percent").toInt(), 100);
    QCOMPARE(job.value("outputs").toList().size(), 2);
    QCOMPARE(job.value("outputs").toList().at(0).toString(),
             QUrl::fromLocalFile(first + QStringLiteral(".out")).toString());
    QFile log(m_log);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList calls = QString::fromUtf8(log.readAll()).trimmed().split(QLatin1Char('\n'));
    QCOMPARE(calls.size(), 2);
    for (int i = 0; i < calls.size(); ++i) {
        QVERIFY(calls.at(i).contains(i == 0 ? first : second));
        QVERIFY(calls.at(i).contains(QStringLiteral("test-model")));
        QCOMPARE(calls.at(i).count(QStringLiteral("--tile-size")), 1);
        QVERIFY(!calls.at(i).contains(QStringLiteral("--auto-tile")));
        QVERIFY(!calls.at(i).contains(QStringLiteral("changed-model")));
    }
    log.close();

    QFile::remove(m_log);
    qputenv("UPIX_TEST_MODE", "success");
    Desk folderDesk;
    const QString folder = m_dir.filePath(QStringLiteral("batch"));
    QVERIFY(QDir().mkpath(folder));
    addImage(QStringLiteral("batch/inside.png"));
    prepareDesk(folderDesk, {folder}, true);
    folderDesk.start();
    QTRY_COMPARE_WITH_TIMEOUT(folderDesk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("done"), 5000);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString folderCall = QString::fromUtf8(log.readAll());
    QCOMPARE(folderCall.count(QStringLiteral("--auto-tile")), 1);
    QCOMPARE(folderCall.count(QStringLiteral("--tile-size")), 0);
    log.close();

    QFile::remove(m_log);
    Desk fileAutoTileDesk;
    const QString fileAutoTile = addImage(QStringLiteral("file-auto.png"));
    prepareDesk(fileAutoTileDesk, {fileAutoTile}, true);
    fileAutoTileDesk.start();
    QTRY_COMPARE_WITH_TIMEOUT(fileAutoTileDesk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("done"), 5000);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString fileAutoTileCall = QString::fromUtf8(log.readAll());
    QCOMPARE(fileAutoTileCall.count(QStringLiteral("--auto-tile")), 0);
    QCOMPARE(fileAutoTileCall.count(QStringLiteral("--tile-size")), 0);
    log.close();
}

void TstDesktopJobs::cancellationStopsQueuedInputs() {
    qputenv("UPIX_TEST_MODE", "cancel");
    Desk desk;
    prepareDesk(desk, {addImage(QStringLiteral("cancel-a.png")), addImage(QStringLiteral("cancel-b.png"))});
    desk.start();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(m_log), 2000);
    desk.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("cancelled"), 3000);
    QFile log(m_log);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(log.readAll()).trimmed().count(QLatin1Char('\n')), 0);
}

void TstDesktopJobs::failureKeepsDiagnosticOutput() {
    qputenv("UPIX_TEST_MODE", "failure");
    Desk desk;
    prepareDesk(desk, {addImage(QStringLiteral("failure-a.png")), addImage(QStringLiteral("failure-b.png"))});
    desk.start();
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("error"), 3000);
    const QString detail = desk.snap().value("job").toMap().value("message").toString();
    QVERIFY(detail.contains(QStringLiteral("diagnostic stderr detail")));
    QFile log(m_log);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(log.readAll()).count(QLatin1Char('\n')), 1);
}

void TstDesktopJobs::liveHostOutputKeepsCardAndPairing() {
    qputenv("UPIX_TEST_MODE", "host");
    Desk desk;
    QVariantMap snap = Desk::fakeSnapshot(QStringLiteral("host"));
    snap["cliFound"] = true;
    desk.applyFakeSnapshot(snap);
    desk.hostStart();
    // The card event carries the v2 invite payload — the QR image source is
    // now a payload string, not scraped half-block art.
    const QString qr = QStringLiteral(
        "{\"v\":2,\"ip\":\"192.0.2.4\",\"port\":43210,\"fp\":\"ABCDEF0123456789\",\"pk\":\"dGVzdA==\"}");
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("host").toMap().value("qr").toString(), qr, 3000);
    const QVariantMap host = desk.snap().value("host").toMap();
    QVERIFY(host.value("running").toBool());
    QCOMPARE(host.value("ips").toList().size(), 1);
    QCOMPARE(host.value("port").toInt(), 43210);
    QCOMPARE(host.value("fingerprint").toString(), QStringLiteral("ABCDEF0123456789"));
    // Log strip still receives human stderr lines.
    QTRY_VERIFY_WITH_TIMEOUT(!host.value("log").toList().isEmpty(), 3000);
    // Pairing arrives as its own event, after a chunked write gap.
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("host").toMap().value("pendingDevice").toString(),
                              QStringLiteral("Pixel 9 Pro"), 3000);
    QCOMPARE(desk.snap().value("host").toMap().value("pendingFp").toString(),
             QStringLiteral("FEDCBA9876543210"));
    desk.hostStop();
}

// Editing the list while a run is live must not touch the run card: resetting
// `job` mid-run made finishJob() see a non-running phase and stop advancing,
// so every input after the current one was silently dropped.
void TstDesktopJobs::runSurvivesInputEdits() {
    qputenv("UPIX_TEST_MODE", "success");
    Desk desk;
    const QString first = addImage(QStringLiteral("live-a.png"));
    const QString second = addImage(QStringLiteral("live-b.png"));
    const QString third = addImage(QStringLiteral("live-c.png"));
    prepareDesk(desk, {first, second});
    desk.start();
    QCOMPARE(desk.snap().value("job").toMap().value("phase").toString(),
             QStringLiteral("running"));

    desk.addPaths({third});
    desk.removeInput(first);
    QCOMPARE(desk.snap().value("job").toMap().value("phase").toString(),
             QStringLiteral("running"));

    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("done"), 5000);
    const QVariantMap job = desk.snap().value("job").toMap();
    QCOMPARE(job.value("index").toInt(), 2);
    QCOMPARE(job.value("total").toInt(), 2);
    QCOMPARE(job.value("outputs").toList().size(), 2);
    // The list edit itself still landed (third added, first removed).
    QCOMPARE(desk.snap().value("inputs").toList().size(), 2);
}

// A second start (Ctrl+Return, or a double CTA tap) used to kill the running
// CLI and restart the queue at index 0.
void TstDesktopJobs::secondStartDuringRunIsIgnored() {
    qputenv("UPIX_TEST_MODE", "cancel");
    Desk desk;
    prepareDesk(desk, {addImage(QStringLiteral("restart-a.png")),
                       addImage(QStringLiteral("restart-b.png"))});
    desk.start();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(m_log), 2000);
    desk.start();
    QTest::qWait(400);
    QFile log(m_log);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(log.readAll()).count(QLatin1Char('\n')), 1);
    log.close();
    QCOMPARE(desk.snap().value("job").toMap().value("index").toInt(), 1);
    desk.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                              QStringLiteral("cancelled"), 3000);
}

// The CLI existing but failing to exec (bad shebang, missing loader, noexec
// mount) reports through waitForStarted(); spawn() used to hold a reference
// into m_procs across that call, which finishJob() emptied.
void TstDesktopJobs::failedToStartSurfacesError() {
    const QString broken = m_dir.filePath(QStringLiteral("broken-upix"));
    QFile script(broken);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("#!/nonexistent/interpreter\n");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    qputenv("UPIX_CLI", broken.toUtf8());
    {
        Desk desk;
        prepareDesk(desk, {addImage(QStringLiteral("broken-a.png"))});
        desk.start();
        QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("job").toMap().value("phase").toString(),
                                  QStringLiteral("error"), 5000);
        QVERIFY(!desk.snap().value("job").toMap().value("message").toString().isEmpty());
    }
    // cleanup() restores UPIX_CLI after every test, so a QVERIFY failure above
    // cannot leak the broken path into the next case.
}

// folderCount/cover are per-input now: removing a folder (or the covering
// entry) restores both instead of leaving a count that includes a folder the
// user just dropped.
void TstDesktopJobs::folderCountAndCoverFollowInputRemoval() {
    Desk desk;
    const QString folder = m_dir.filePath(QStringLiteral("coverbatch"));
    QVERIFY(QDir().mkpath(folder));
    const QString inside = addImage(QStringLiteral("coverbatch/a.png"));
    addImage(QStringLiteral("coverbatch/b.png"));
    const QString single = addImage(QStringLiteral("cover-single.png"));
    prepareDesk(desk, {folder, single});
    QCOMPARE(desk.snap().value("folderCount").toInt(), 3);
    QCOMPARE(desk.snap().value("cover").toString(),
             QUrl::fromLocalFile(inside).toString());

    desk.removeInput(folder);
    QCOMPARE(desk.snap().value("folderCount").toInt(), 1);
    QCOMPARE(desk.snap().value("cover").toString(),
             QUrl::fromLocalFile(single).toString());
}

// A drop that yields nothing (unsupported suffix, vanished path) must say so;
// before, the picker/drop just did nothing.
void TstDesktopJobs::unsupportedDropIsReported() {
    Desk desk;
    prepareDesk(desk, {});
    const QString text = m_dir.filePath(QStringLiteral("notes.txt"));
    QFile f(text);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("not an image");
    f.close();

    desk.addPaths({text, m_dir.filePath(QStringLiteral("vanished.png"))});
    QVERIFY(desk.snap().value("inputs").toList().isEmpty());
    QCOMPARE(desk.snap().value("notice").toString(),
             QStringLiteral("已忽略 2 个不支持的文件"));
}

// The real boot path (not fixture mode) probes `--version`: a CLI older than
// the desktop's machine-readable contract (UPIX_PROGRESS=json, post cli-v0.1.0)
// must surface cliCompatible=false for the HostPage warning; same-repo builds
// and newer CLIs pass.
void TstDesktopJobs::cliVersionGateFlagsOldCliAndPassesNew() {
    qputenv("UPIX_TEST_CLI_VERSION", "0.1.0");
    qputenv("UPIX_FAKE", "");  // empty = run the real boot path
    Desk desk;
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("cliFound"), QVariant(true), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(desk.snap().value("cliCompatible"), QVariant(false), 3000);
    QVERIFY(desk.snap().value("cliVersion").toString().startsWith(QStringLiteral("v0.1.0")));

    qputenv("UPIX_TEST_CLI_VERSION", "0.2.0");
    Desk newer;
    QTRY_COMPARE_WITH_TIMEOUT(newer.snap().value("cliCompatible"), QVariant(true), 3000);
}

} // namespace

QTEST_MAIN(TstDesktopJobs)
#include "tst_desk_jobs.moc"
