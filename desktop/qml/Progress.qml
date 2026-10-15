import QtQuick

// Slim determinate progress: 3px track, primary fill, 300ms linear width.
// Stock ProgressBar paints its fill from palette.dark/mid (outlineV here),
// which read as a disabled bar rather than progress.
Item {
    id: bar
    property real value: 0
    property real from: 0
    property real to: 100
    // Screen-reader name for the bar (the visible caption is a sibling Text).
    property string label: ""
    implicitHeight: 3

    Accessible.role: Accessible.ProgressBar
    Accessible.name: bar.label
    Accessible.description: Math.round(bar.norm * 100) + "%"

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
