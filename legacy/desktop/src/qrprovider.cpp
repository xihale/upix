#include "qrprovider.h"
#include "qrcodegen.hpp"
#include <QUrl>

using qrcodegen::QrCode;

// The card event carries the v2 invite payload JSON; encode it directly.
static QImage imageFromText(const QString& text) {
    const QrCode qr = QrCode::encodeText(text.toUtf8().constData(),
                                         QrCode::Ecc::MEDIUM);
    const int border = 2;
    const int dim = qr.getSize() + border * 2;
    QImage img(dim, dim, QImage::Format_RGB32);
    img.fill(0xFFFFFFFF);
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y)) img.setPixel(x + border, y + border, 0xFF000000);
    return img;
}

QrProvider::QrProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage QrProvider::requestImage(const QString& id, QSize* size,
                                const QSize& requested) {
    const QString payload = QUrl::fromPercentEncoding(id.toUtf8());
    QImage img = imageFromText(payload);

    if (!requested.isEmpty() && requested.width() > 0 && requested.height() > 0 &&
        (requested.width() != img.width() || requested.height() != img.height()))
        img = img.scaled(requested, Qt::KeepAspectRatio,
                         Qt::TransformationMode::FastTransformation);
    if (size) *size = img.size();
    return img;
}
