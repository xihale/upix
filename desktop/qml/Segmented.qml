import QtQuick

// Segmented control (output format). Etched track + raised indicator chip,
// sliding 220ms.
//
// Geometry is pure binding: selIndex recomputes off `items`/`current`, and
// indicator.x/width track it. The old imperative place() (signal handlers +
// Component.onCompleted writing x) was order-flaky — it computed the wrong
// cell or never ran at all depending on when bindings attached, which read
// as "输出只能选择跟随".
Rectangle {
    id: seg
    property var items: [] // [{label, value}]
    property string current: ""
    signal picked(string value)

    readonly property int selIndex: {
        var idx = 0
        for (var i = 0; i < items.length; i++)
            if (items[i].value === current) idx = i
        return idx
    }

    height: 36
    radius: 9
    color: Theme.containerLow
    border.width: 1
    border.color: Theme.outlineV

    Rectangle {
        id: indicator
        y: 3
        width: Math.max(0, (seg.width - 6) / (seg.items.length || 1))
        x: 3 + seg.selIndex * width
        height: 30
        radius: 7
        color: Theme.high
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] } }
        Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] } }
    }

    Row {
        anchors.fill: parent
        anchors.margins: 3
        Repeater {
            model: seg.items
            delegate: Item {
                width: (seg.width - 6) / (seg.items.length || 1)
                height: 30
                Text {
                    anchors.centerIn: parent
                    text: modelData.label
                    font.pixelSize: 13
                    color: seg.current === modelData.value ? Theme.onSurface : Theme.muted
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: seg.picked(modelData.value)
                }
            }
        }
    }
}
