#include "icons.h"

#include <QColor>
#include <QPainter>
#include <QUrl>
#include <QSvgRenderer>

namespace {

// Inner SVG markup of the used Lucide icons (lucide-static 0.544.0, ISC),
// verbatim minus the <svg> wrapper. All geometry draws in a 24x24 viewBox and
// inherits stroke/fill from the generated root element.
const QHash<QString, QString>& lucidePaths()
{
    static const QHash<QString, QString> paths = {
        {QStringLiteral("check"), QStringLiteral(R"(<path d="M20 6 9 17l-5-5" />)")},
        {QStringLiteral("chevron-down"), QStringLiteral(R"(<path d="m6 9 6 6 6-6" />)")},
        {QStringLiteral("copy"), QStringLiteral(R"(<rect width="14" height="14" x="8" y="8" rx="2" ry="2" /> <path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2" />)")},
        {QStringLiteral("download"), QStringLiteral(R"(<path d="M12 15V3" /> <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" /> <path d="m7 10 5 5 5-5" />)")},
        {QStringLiteral("folder"), QStringLiteral(R"(<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z" />)")},
        {QStringLiteral("folder-open"), QStringLiteral(R"(<path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2" />)")},
        {QStringLiteral("image"), QStringLiteral(R"(<rect width="18" height="18" x="3" y="3" rx="2" ry="2" /> <circle cx="9" cy="9" r="2" /> <path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21" />)")},
        {QStringLiteral("layers"), QStringLiteral(R"(<path d="M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83z" /> <path d="M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12" /> <path d="M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17" />)")},
        {QStringLiteral("play"), QStringLiteral(R"(<path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z" />)")},
        {QStringLiteral("plus"), QStringLiteral(R"(<path d="M5 12h14" /> <path d="M12 5v14" />)")},
        {QStringLiteral("qr-code"), QStringLiteral(R"(<rect width="5" height="5" x="3" y="3" rx="1" /> <rect width="5" height="5" x="16" y="3" rx="1" /> <rect width="5" height="5" x="3" y="16" rx="1" /> <path d="M21 16h-3a2 2 0 0 0-2 2v3" /> <path d="M21 21v.01" /> <path d="M12 7v3a2 2 0 0 1-2 2H7" /> <path d="M3 12h.01" /> <path d="M12 3h.01" /> <path d="M12 16v.01" /> <path d="M16 12h1" /> <path d="M21 12v.01" /> <path d="M12 21v-1" />)")},
        {QStringLiteral("refresh-cw"), QStringLiteral(R"(<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8" /> <path d="M21 3v5h-5" /> <path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16" /> <path d="M8 16H3v5" />)")},
        {QStringLiteral("smartphone"), QStringLiteral(R"(<rect width="14" height="20" x="5" y="2" rx="2" ry="2" /> <path d="M12 18h.01" />)")},
        {QStringLiteral("x"), QStringLiteral(R"(<path d="M18 6 6 18" /> <path d="m6 6 12 12" />)")},
        {QStringLiteral("zoom-in"), QStringLiteral(R"(<circle cx="11" cy="11" r="8" /> <line x1="21" x2="16.65" y1="21" y2="16.65" /> <line x1="11" x2="11" y1="8" y2="14" /> <line x1="8" x2="14" y1="11" y2="11" />)")},
    };
    return paths;
}

} // namespace

IconProvider::IconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage IconProvider::requestImage(const QString& id, QSize* size,
                                  const QSize& requestedSize)
{
    // id = "<kind>/<urlenc-color>[?fill=1]" — everything after "image://icon/"
    const QUrl url(QStringLiteral("image://icon/") + id);
    const QString kind = url.path().section(QLatin1Char('/'), 1, 1);
    const QColor color = QColor(
        QUrl::fromPercentEncoding(url.path().section(QLatin1Char('/'), 2).toUtf8()));
    const bool filled = url.query() == QLatin1String("fill=1");

    const QString markup = lucidePaths().value(kind);
    if (markup.isEmpty() || !color.isValid())
        return QImage();

    // Opaque 6-digit hex: QSvgRenderer does not parse CSS-color #AARRGGBB.
    const QString hex = color.name();
    const QString root = filled
        ? QStringLiteral(
              R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="%1" stroke="%1" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)")
              .arg(hex)
        : QStringLiteral(
              R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="%1" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)")
              .arg(hex);
    const QByteArray svg = (root + markup + QStringLiteral("</svg>")).toUtf8();

    QSize px = requestedSize;
    if (px.isEmpty()) px = QSize(48, 48);
    QImage img(px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QSvgRenderer renderer(svg);
    QPainter p(&img);
    renderer.render(&p, QRectF(0, 0, px.width(), px.height()));
    p.end();

    if (size) *size = px;
    return img;
}
