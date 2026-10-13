import QtQuick

// Slim determinate progress: 3px track, primary fill, 300ms linear width.
// Stock ProgressBar paints its fill from palette.dark/mid (outlineV here),
// which read as a disabled bar rather than progress.
Item {
    id: bar
    property real value: 0
    property real from: 0
    property real to: 100
    implicitHeight: 3

    readonly property real norm: to > from
        ? Math.min(1, Math.max(0, (value - from) / (to - from))) : 0

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 3
        radius: 1.5
        color: Theme.highest
    }
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width * bar.norm
        height: 3
        radius: 1.5
        color: Theme.primary
        Behavior on width { NumberAnimation { duration: 300 } }
    }
}
