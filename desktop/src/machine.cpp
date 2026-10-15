#include "machine.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace machine {

QVariantMap parseEvent(const QString& line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
    if (!doc.isObject()) return {};
    const QJsonObject obj = doc.object();
    // Streamed events are keyed by "t"; one-shot documents (model list,
    // daemon status, import result) arrive as whole documents and are parsed
    // by their callers — not here.
    if (!obj.value(QStringLiteral("t")).isString()) return {};
    return obj.toVariantMap();
}

QString daemonLabel(const QVariantMap& status) {
    if (status.value(QStringLiteral("running")).toBool()) {
        QString label = QStringLiteral("running (pid %1, socket %2)")
                            .arg(status.value(QStringLiteral("pid")).toInt())
                            .arg(status.value(QStringLiteral("socket")).toString());
        if (status.value(QStringLiteral("socketMissing")).toBool())
            label += QStringLiteral(" — socket file missing!");
        return label;
    }
    return status.value(QStringLiteral("stale")).toBool()
               ? QStringLiteral("not running (stale pid file)")
               : QStringLiteral("not running");
}

QString doneNote(const QString& message) {
    return QStringLiteral("完成 · ") + message;
}

}  // namespace machine
