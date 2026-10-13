import QtQuick

// 并行 stepper row: [-] n [+]
Item {
    id: row
    property int value: 4
    signal changed(int value)

    height: 40

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: "并行"
        font.pixelSize: 14
        color: Theme.onSurface
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Rectangle {
            width: 30; height: 30; radius: 8
            color: maMinus.pressed ? Theme.highest : maMinus.containsMouse ? Theme.high : "transparent"
            border.width: 1
            border.color: Theme.outlineV
            Text { anchors.centerIn: parent; text: "−"; font.pixelSize: 15; color: Theme.onSurface }
            MouseArea {
                id: maMinus
                anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: row.changed(row.value - 1)
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: row.value
            width: 18
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 14
            font.family: Theme.mono
            color: Theme.onSurface
        }
        Rectangle {
            width: 30; height: 30; radius: 8
            color: maPlus.pressed ? Theme.highest : maPlus.containsMouse ? Theme.high : "transparent"
            border.width: 1
            border.color: Theme.outlineV
            Text { anchors.centerIn: parent; text: "+"; font.pixelSize: 15; color: Theme.onSurface }
            MouseArea {
                id: maPlus
                anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: row.changed(row.value + 1)
            }
        }
    }
}
