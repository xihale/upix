import QtQuick

// 并行 stepper row: [-] n [+]. The two plate buttons are focusable (Tab) and
// carry a screen-reader name; the row itself only lays out the label.
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

    component StepButton: Item {
        id: step
        property string glyph: ""
        property string accessibleName: ""
        signal clicked()

        width: 30
        height: 30
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: step.accessibleName
        Accessible.onPressAction: step.clicked()

        Rectangle {
            anchors.fill: parent
            radius: 8
            color: ma.pressed ? Theme.highest : ma.containsMouse ? Theme.high : "transparent"
            border.width: 1
            border.color: step.activeFocus ? Theme.primary : Theme.outlineV
        }
        Text {
            anchors.centerIn: parent
            text: step.glyph
            font.pixelSize: 15
            color: Theme.onSurface
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: step.clicked()
        }

        Keys.onReturnPressed: step.clicked()
        Keys.onEnterPressed: step.clicked()
        Keys.onSpacePressed: step.clicked()
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        StepButton {
            glyph: "−"
            accessibleName: "减少并行数"
            onClicked: row.changed(row.value - 1)
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
        StepButton {
            glyph: "+"
            accessibleName: "增加并行数"
            onClicked: row.changed(row.value + 1)
        }
    }
}
