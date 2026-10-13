import QtQuick
import QtQuick.Window

// Library icon (Lucide, ISC) rasterized by the C++ `icon` provider and tinted
// via the URL: image://icon/<kind>/<color>[?fill=1]. Same API as the old
// hand-drawn Canvas set (kind/ink/fill) — callers only change kind names.
Image {
    id: ic
    property string kind: "image"
    property color ink: Theme.onSurface
    property bool fill: false

    asynchronous: false
    fillMode: Image.PreserveAspectFit
    source: kind === "" ? ""
        : "image://icon/" + kind + "/" + encodeURIComponent(String(ink)) + (fill ? "?fill=1" : "")
    // Render at device pixels so icons stay crisp on scaled displays.
    sourceSize: Qt.size(Math.max(1, Math.ceil(width * (Screen.devicePixelRatio || 1))),
                        Math.max(1, Math.ceil(height * (Screen.devicePixelRatio || 1))))
}
