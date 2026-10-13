#include "qrprovider.h"
#include "qrcodegen.hpp"
#include <QRegularExpression>
#include <QUrl>

using qrcodegen::QrCode;

static bool isArtChar(QChar c) {
    return c == QChar(0x2580) /*▀*/ || c == QChar(0x2584) /*▄*/ ||
           c == QChar(0x2588) /*█*/ || c == QLatin1Char(' ');
}

// Half-block art -> module matrix. Each char holds two vertical modules:
// ▀ top dark, ▄ bottom dark, █ both, space none. Lines include the quiet
// zone the CLI printed, so the matrix is the exact QR the Android app sees.
static QImage imageFromArt(const QString& art) {
    const QStringList lines = art.split(QLatin1Char('\n'));
    int w = 0;
    for (const QString& l : lines) w = qMax(w, l.size());
    const int h = lines.size() * 2;
    if (w <= 0 || h <= 0) return QImage();

    QImage img(w, h, QImage::Format_RGB32);
    img.fill(0xFFFFFFFF);
    for (int y = 0; y < lines.size(); ++y) {
        const QString& line = lines[y];
        for (int x = 0; x < w; ++x) {
            const QChar c = x < line.size() ? line.at(x) : QLatin1Char(' ');
            if (c == QChar(0x2580) || c == QChar(0x2588))
                img.setPixel(x, y * 2, 0xFF000000);
            if (c == QChar(0x2584) || c == QChar(0x2588))
                img.setPixel(x, y * 2 + 1, 0xFF000000);
        }
    }
    return img;
}

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

    // Art form: art chars occupy whole lines; text payloads have no ▀▄█.
    static const QRegularExpression artProbe(QStringLiteral("[\u2580\u2584\u2588]"));
    QImage img;
    if (artProbe.match(payload).hasMatch()) {
        // Keep only art lines; drop log noise around the block.
        QStringList art;
        for (const QString& l : payload.split(QLatin1Char('\n'))) {
            if (l.isEmpty()) continue;
            bool artLine = true;
            for (const QChar c : l)
                if (!isArtChar(c)) { artLine = false; break; }
            if (artLine) art.append(l);
        }
        img = imageFromArt(art.join(QLatin1Char('\n')));
    }
    if (img.isNull()) img = imageFromText(payload);

    if (!requested.isEmpty() && requested.width() > 0 && requested.height() > 0 &&
        (requested.width() != img.width() || requested.height() != img.height()))
        img = img.scaled(requested, Qt::KeepAspectRatio,
                         Qt::TransformationMode::FastTransformation);
    if (size) *size = img.size();
    return img;
}
