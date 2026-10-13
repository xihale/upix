// Line parsers for upix-cli output. Every regex here is grounded in the real
// CLI (verified against `upix model list/list-remote`, `daemon status`,
// `host info/start`, and a live UPIX_PROGRESS=1 run).
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace parser {

// `upix --version` -> "v0.1.0 avif:system-dlopen" (verbatim first line).
QString parseVersion(const QString& line);

// `upix model list`:
//   Installed models (/path/to/models):
//     Name (4x, 31 MB) [动漫] ★
QVariantMap parseModelList(const QStringList& lines);   // {modelsDir, installed:[{name,scale,size,scene,recommended}]}

// `upix model list-remote`:
//   Models available in the release (<url>):
//     Name (4x, 29.5 MB) [照片]
QVariantList parseRemoteList(const QStringList& lines); // [{name,scale,size,scene,recommended}]

// `upix daemon status`: "not running" | "running (pid 42, socket /x)"
QVariantMap parseDaemonStatus(const QString& line);     // {running,label}

struct HostCard {
    QStringList ips;        // card order; first is "(default for QR)"
    int port = 0;
    QString fingerprint;
    QString qrArt;          // ASCII half-block QR block, verbatim lines
    bool valid = false;
};
// `upix host info` / the card printed by `upix host start`.
HostCard parseHostCard(const QStringList& lines);

// Pairing prompt inside `host start` output:
//   │ 设备: Pixel 9 Pro
//   │ 指纹: EC61F2B66D4C25C4
bool parsePairing(const QString& line, QString* device, QString* fp);

// Progress line from a live run:  `42% (5/12, ETA 3s)`  /  `8% (1/12)`
// tiles-or-files; single image => tiles, directory => tiles within current
// file. Returns percent/done/total/eta keys; done=-1 when not a match.
QVariantMap parseProgress(const QString& line);

// `[Engine] [INFO] Output saved (via warm daemon): /path/out.png`
QString parseSaved(const QString& line);

// `Downloading Name (4x, 29.5 MB) ... 10% 20% ... done` -> latest percent, -1 none.
int parseDownloadPercent(const QString& line);

// `[Host] [INFO] Task progress: 57%` -> -1 when absent.
int parseHostTaskPercent(const QString& line);

// `[2026-..] [Task] [INFO] Input path: /a/b.png ...` -> current file stem, "" none.
QString parseCurrentFile(const QString& line);

// "Processing complete: 2560x1680" and friends -> done-note text, "" none.
QString parseDoneNote(const QString& line);

} // namespace parser
