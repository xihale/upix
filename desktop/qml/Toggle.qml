import QtQuick

// Row with label + custom switch. CSS: 44x26 track, 20px thumb, 180ms ease.
Row {
    id: row
    property string label
    property bool checked: false
    signal toggled(bool checked)

    spacing: 12
    height: 44

    Item {
        width: parent.width - switchTrack.width - 12
        height: parent.height
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: row.label
            font.pixelSize: 16
            color: Theme.onSurface
        }
    }

    Rectangle {
        id: switchTrack
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        height: 26
        radius: 13
        color: row.checked ? Theme.primary : Theme.highest
        Behavior on color { ColorAnimation { duration: 180 } }

        Rectangle {
            x: row.checked ? 21 : 3
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            radius: 10
            color: row.checked ? Theme.onPrimary : Theme.onSurface
            Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] } }
            Behavior on color { ColorAnimation { duration: 180 } }
        }

        MouseArea {
            anchors.fill: parent
            anchors.margins: -10
            cursorShape: Qt.PointingHandCursor
            onClicked: row.toggled(!row.checked)
        }
    }
}
