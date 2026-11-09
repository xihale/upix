// Theme tokens as a QML singleton. Follows the system color scheme,
// overridable with UPIX_THEME=light|dark. One `changed` signal re-tints QML
// (see the zone Canvas in Workbench.qml).
#pragma once
#include <QColor>
#include <QObject>
#include <QQuickImageProvider>

class QGuiApplication;

class Theme : public QObject {
    Q_OBJECT

    // Core tokens (see DESIGN.md); light values in comments.
    // Darkroom-instrument palette: warm graphite neutrals, one cold teal signal
    // (the brand accent — docs/DESIGN_LANGUAGE.md §1, the same two values the
    // Android fallback palette ships), coral only for errors.
    Q_PROPERTY(QColor bg READ bg NOTIFY changed)                  // #131316 / #f4f3ef
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)        // #1a1a1f / #ffffff
    Q_PROPERTY(QColor container READ container NOTIFY changed)    // #202026 / #ecebe6
    Q_PROPERTY(QColor containerLow READ containerLow NOTIFY changed)
    Q_PROPERTY(QColor high READ high NOTIFY changed)
    Q_PROPERTY(QColor highest READ highest NOTIFY changed)        // #323239 / #d9d8d1
    Q_PROPERTY(QColor onSurface READ onSurface NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)            // #a2a2ac / #5e5d57
    Q_PROPERTY(QColor outlineV READ outlineV NOTIFY changed)
    Q_PROPERTY(QColor primary READ primary NOTIFY changed)        // #7FDAC9 / #0E6B5F
    Q_PROPERTY(QColor onPrimary READ onPrimary NOTIFY changed)    // #00382F / #ffffff
    Q_PROPERTY(QColor error READ error NOTIFY changed)            // #ff7361 / #c23a2b
    Q_PROPERTY(QColor errorC READ errorC NOTIFY changed)          // error banner bg
    Q_PROPERTY(QColor onErrorC READ onErrorC NOTIFY changed)
    Q_PROPERTY(QColor inverse READ inverse NOTIFY changed)        // toast bg
    Q_PROPERTY(QColor onInverse READ onInverse NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString mono READ mono CONSTANT FINAL)             // metadata font family

public:
    explicit Theme(QObject* parent = nullptr);

    // QML singleton plumbing: reuse one instance for C++ access.
    static Theme* instance();

    QColor bg() const { return m_dark ? "#131316" : "#f4f3ef"; }
    QColor surface() const { return m_dark ? "#1a1a1f" : "#ffffff"; }
    QColor container() const { return m_dark ? "#202026" : "#ecebe6"; }
    QColor containerLow() const { return m_dark ? "#1c1c21" : "#f0efeb"; }
    QColor high() const { return m_dark ? "#26262c" : "#e3e2dc"; }
    QColor highest() const { return m_dark ? "#323239" : "#d9d8d1"; }
    QColor onSurface() const { return m_dark ? "#ececee" : "#232219"; }
    // Light muted was #71706a: 4.16:1 on container, 4.32:1 on container-low,
    // 4.47:1 on bg — below WCAG AA (4.5:1) for the 12–13px metadata it paints.
    // #5e5d57 keeps the warm-neutral hue and clears 4.5:1 on every light
    // surface (worst pair 4.62:1 on `highest`).
    QColor muted() const { return m_dark ? "#a2a2ac" : "#5e5d57"; }
    QColor outlineV() const { return m_dark ? "#2d2d34" : "#d6d5cd"; }
    QColor primary() const { return m_dark ? "#7FDAC9" : "#0E6B5F"; }
    QColor onPrimary() const { return m_dark ? "#00382F" : "#ffffff"; }
    QColor error() const { return m_dark ? "#ff7361" : "#c23a2b"; }
    QColor errorC() const { return m_dark ? "#33201d" : "#f9e9e6"; }
    QColor onErrorC() const { return m_dark ? "#ffb3a8" : "#c23a2b"; }
    QColor inverse() const { return m_dark ? "#ececee" : "#232219"; }
    QColor onInverse() const { return m_dark ? "#131316" : "#f4f3ef"; }
    bool dark() const { return m_dark; }
    QString mono() const { return QStringLiteral("monospace"); }

    void setDark(bool dark);

signals:
    void changed();

private:
    static bool detectScheme();
    bool m_dark;
};
