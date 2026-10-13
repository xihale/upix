// Lucide icon rasterizer for QML: image://icon/<kind>/<urlenc-color>[?fill=1].
// Paths from lucide-static (ISC license), rendered by QtSvg and tinted per
// request; QML Image caches by URL, so each (kind, color, fill) triple renders
// once.
#pragma once
#include <QQuickImageProvider>

class IconProvider : public QQuickImageProvider {
public:
    IconProvider();
    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requestedSize) override;
};
