import QtQuick

// Instrument slider: 2px track, primary fill, ruled ticks under the groove,
// ringed thumb. Used for 质量/分块.
Item {
    id: slider
    property real from: 1
    property real to: 100
    property real value: from
    property real step: 1
    property bool ticks: true
    signal moved(real value)

    height: 30

    function norm() {
        if (to <= from) return 0
        return Math.min(1, Math.max(0, (value - from) / (to - from)))
    }

    // Ruler ticks — the enlarger's scale. Aligned with the thumb travel
    // (8 .. width-8) so the ends read as stops, not decoration.
    Row {
        visible: slider.ticks
        anchors.top: parent.top
        anchors.topMargin: 22
        x: 8
        spacing: (slider.width - 16) / 10 - 1
        Repeater {
            model: 11
            Rectangle {
                width: 1
                height: index % 5 === 0 ? 6 : 3
                color: Theme.outlineV
            }
        }
    }

    Rectangle {
        anchors.verticalCenterOffset: -2
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 2
        color: Theme.outlineV
    }
    Rectangle {
        anchors.verticalCenterOffset: -2
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width * norm()
        height: 2
        color: Theme.primary
    }
    Rectangle {
        id: thumb
        anchors.verticalCenterOffset: -2
        anchors.verticalCenter: parent.verticalCenter
        x: parent.width * norm() - 7
        width: 14
        height: 14
        radius: 7
        color: Theme.onSurface
        border.width: 3
        border.color: Theme.bg
    }

    function apply(x) {
        var t = Math.min(1, Math.max(0, (x - 8) / (slider.width - 16 + 0.0001)))
        var raw = slider.from + t * (slider.to - slider.from)
        var v = Math.round(raw / slider.step) * slider.step
        v = Math.min(slider.to, Math.max(slider.from, v))
        slider.value = v
        slider.moved(v)
    }

    MouseArea {
        anchors.fill: parent
        anchors.margins: -8
        cursorShape: Qt.PointingHandCursor
        onPressed: (mouse) => slider.apply(mouse.x)
        onPositionChanged: (mouse) => { if (pressed) slider.apply(mouse.x) }
    }
}
