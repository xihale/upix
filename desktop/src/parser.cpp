#include "parser.h"
#include <QFileInfo>
#include <QRegularExpression>

namespace parser {

static const QRegularExpression kPairLine(
    QStringLiteral("^\\s+([^\\s].*?)\\s+\\((\\d+)x,\\s*([\\d.]+)\\s*MB\\)"
                  // optional scene annotation + recommended star:
                  // "  AnimeSharp-x4 (4x, 31 MB) [动漫] ★"
                  "(?:\\s+\\[([^\\]]+)\\])?(?:\\s+(★))?$"));

QString parseVersion(const QString& line) {
    return line.trimmed();
}

static QVariantMap modelEntry(const QRegularExpressionMatch& m) {
    return QVariantMap{
        {"name", m.captured(1)},
        {"scale", m.captured(2).toInt()},
        {"size", m.captured(3) + QStringLiteral(" MB")},
        {"scene", m.captured(4)},   // "" for non-catalog (imported) models
        {"recommended", !m.captured(5).isEmpty()},
    };
}

QVariantMap parseModelList(const QStringList& lines) {
    QVariantMap out;
    QVariantList installed;
    static const QRegularExpression header(
        QStringLiteral("^Installed models \\((.+)\\):$"));
    for (const QString& line : lines) {
        const auto h = header.match(line.trimmed());
        if (h.hasMatch()) {
            out.insert(QStringLiteral("modelsDir"), h.captured(1));
            continue;
        }
        const auto m = kPairLine.match(line);
        if (m.hasMatch()) installed.append(modelEntry(m));
    }
    out.insert(QStringLiteral("installed"), installed);
    return out;
}

QVariantList parseRemoteList(const QStringList& lines) {
    QVariantList remote;
    for (const QString& line : lines) {
        const auto m = kPairLine.match(line);
        if (m.hasMatch()) remote.append(modelEntry(m));
    }
    return remote;
}

QVariantMap parseDaemonStatus(const QString& line) {
    const QString t = line.trimmed();
    // "running (pid 42, socket /path/serve.sock)" | "not running" |
    // "not running (stale pid file)" | "running (… socket file missing!)".
    // The socket path is routinely 40+ chars; the UI shows `pid`/`socketName`
    // compactly and keeps the raw line for the tooltip.
    static const QRegularExpression pidRe(QStringLiteral("pid (\\d+)"));
    static const QRegularExpression sockRe(QStringLiteral("socket ([^)\\s]+)"));
    const auto pidMatch = pidRe.match(t);
    const auto sockMatch = sockRe.match(t);
    const QString sockPath = sockMatch.hasMatch() ? sockMatch.captured(1) : QString();
    return QVariantMap{
        {"running", t.startsWith(QStringLiteral("running"))},
        {"label", t},
        {"pid", pidMatch.hasMatch() ? pidMatch.captured(1).toInt() : 0},
        {"socketName", sockPath.isEmpty() ? QString()
                                          : QFileInfo(sockPath).fileName()},
        {"socketMissing", t.contains(QStringLiteral("socket file missing"))},
    };
}

HostCard parseHostCard(const QStringList& lines) {
    HostCard card;
    bool inCard = false, inQr = false;
    for (const QString& raw : lines) {
        const QString line = raw;
        if (line.contains(QStringLiteral("Connection Information:"))) {
            inCard = true;
            continue;
        }
        if (!inCard) continue;
        if (line.startsWith(QStringLiteral("  IP: "))) {
            card.ips.append(line.section(QStringLiteral("  "), 1, 1).mid(5).trimmed());
            continue;
        }
        if (line.startsWith(QStringLiteral("  Port: "))) {
            card.port = line.mid(8).trimmed().toInt();
            continue;
        }
        if (line.startsWith(QStringLiteral("  Fingerprint: "))) {
            card.fingerprint = line.mid(15).trimmed();
            card.valid = true;
            continue;
        }
        if (line.contains(QStringLiteral("QR Code"))) {
            inQr = true;
            inCard = false;
            continue;
        }
        if (inQr) {
            static const QRegularExpression art(
                QStringLiteral("[\u2580\u2584\u2588]")); // ▀ ▄ █
            if (art.match(line).hasMatch())
                card.qrArt += line + QLatin1Char('\n');
            else if (!card.qrArt.isEmpty())
                break; // first non-art line after the block ends it
        }
    }
    while (card.qrArt.endsWith(QLatin1Char('\n'))) card.qrArt.chop(1);
    return card;
}

bool parsePairing(const QString& line, QString* device, QString* fp) {
    static const QRegularExpression dev(QStringLiteral("^│\\s*设备:\\s*(.+)$"));
    static const QRegularExpression fpr(QStringLiteral("^│\\s*指纹:\\s*(.+)$"));
    const auto d = dev.match(line.trimmed());
    if (d.hasMatch()) {
        *device = d.captured(1).trimmed();
        return true;
    }
    const auto f = fpr.match(line.trimmed());
    if (f.hasMatch()) {
        *fp = f.captured(1).trimmed();
        return true;
    }
    return false;
}

QVariantMap parseProgress(const QString& line) {
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)% \\((\\d+)/(\\d+)(?:, ETA (\\d+)s)?\\)"));
    const auto m = re.match(line.trimmed());
    if (!m.hasMatch())
        return QVariantMap{{"percent", -1}};
    return QVariantMap{
        {"percent", m.captured(1).toInt()},
        {"done", m.captured(2).toInt()},
        {"total", m.captured(3).toInt()},
        {"eta", m.hasCaptured(4) ? m.captured(4).toInt() : 0},
    };
}

QString parseSaved(const QString& line) {
    static const QRegularExpression re(
        QStringLiteral("Output saved[^:]*:\\s*(.+)$"));
    const auto m = re.match(line);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

int parseDownloadPercent(const QString& line) {
    static const QRegularExpression re(QStringLiteral("(\\d+)%"));
    int last = -1;
    auto it = re.globalMatch(line);
    while (it.hasNext()) last = it.next().captured(1).toInt();
    return last;
}

int parseHostTaskPercent(const QString& line) {
    static const QRegularExpression re(QStringLiteral("Task progress:\\s*(\\d+)%"));
    const auto m = re.match(line);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

QString parseCurrentFile(const QString& line) {
    static const QRegularExpression re(QStringLiteral("Input path:\\s*(.+?)\\s"));
    const auto m = re.match(line);
    if (!m.hasMatch()) return QString();
    return QRegularExpression(QStringLiteral("^.*/")).match(m.captured(1))
               .hasMatch()
           ? m.captured(1).section(QLatin1Char('/'), -1)
           : m.captured(1);
}

QString parseDoneNote(const QString& line) {
    static const QRegularExpression re(QStringLiteral("Processing complete:\\s*(.+)$"));
    const auto m = re.match(line);
    return m.hasMatch() ? QStringLiteral("完成 · ") + m.captured(1).trimmed()
                        : QString();
}

} // namespace parser
