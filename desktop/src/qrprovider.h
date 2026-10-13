// image://qr/<payload> — renders the phone-pairing QR.
// Payload is percent-encoded by QML. Two accepted forms:
//  1. the CLI's ASCII half-block card (█▀▄/space), parsed module-exact;
//  2. any text (e.g. the v2 JSON card) generated via the vendored Nayuki lib.
#pragma once
#include <QQuickImageProvider>

class QrProvider : public QQuickImageProvider {
public:
    QrProvider();
    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requested) override;
};
