// image://qr/<payload> — renders the phone-pairing QR.
// The payload is percent-encoded by QML (HostPage passes encodeURIComponent of
// the card JSON) and decoded here, then encoded as an ECC-M symbol by the
// vendored Nayuki lib. Any text works; the pre-v2 ASCII half-block card
// (█▀▄/space) is not parsed.
#pragma once
#include <QQuickImageProvider>

class QrProvider : public QQuickImageProvider {
public:
    QrProvider();
    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requested) override;
};
