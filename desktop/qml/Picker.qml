import QtQuick
import QtQuick.Controls.Basic

// Collapsed picker row + token-styled popup list. Replaces stock ComboBox: its
// Basic-style popup renders with library chrome/metrics and would not take the
// container/radius language. items: [{name, short, scale, size}].
Item {
    id: picker
    property var items: []
    property string current: ""
    property string placeholder: "选择模型"
    signal picked(string name)

    height: 40
    readonly property int index: {
        for (var i = 0; i < items.length; i++)
            if (items[i].name === current) return i
        return -1
    }
    readonly property var selected: index >= 0 ? items[index] : null

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: hover.hovered || pop.visible ? Theme.high : "transparent"
        border.width: 1
        border.color: pop.visible ? Theme.muted : Theme.outlineV
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.right: chev.left
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, picker.width - 96)
            // The full model name, not the family-stripped variant: with the
            // name already carrying "-x4" the old suffix printed "AnimeSharp-x4
            // x4". Only show the scale when the name does not state it.
            text: picker.selected
                  ? picker.selected.name
                  : (picker.items.length ? picker.placeholder : "未发现模型")
            elide: Text.ElideRight
            font.pixelSize: 14
            color: picker.selected ? Theme.onSurface : Theme.muted
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: picker.selected && !/-x\d+/i.test(picker.selected.name)
            text: picker.selected ? "x" + picker.selected.scale : ""
            font.pixelSize: 12
            font.family: Theme.mono
            color: Theme.muted
        }
    }

    Icon {
        id: chev
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: 16; height: 16
        kind: "chevron-down"
        ink: Theme.muted
        rotation: pop.visible ? 180 : 0
        Behavior on rotation {
            NumberAnimation { duration: 180; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] }
        }
    }

    TapHandler { onTapped: pop.visible ? pop.close() : pop.open() }

    Popup {
        id: pop
        y: picker.height + 6
        width: picker.width
        padding: 4

        background: Rectangle {
            radius: 10
            color: Theme.surface
            border.color: Theme.outlineV
            border.width: 1
        }

        contentItem: ListView {
            id: list
            implicitHeight: Math.min(contentHeight, 300)
            clip: true
            model: picker.items
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            delegate: Rectangle {
                required property var modelData
                required property int index
                width: list.width
                height: 44
                radius: 6
                color: index === picker.index ? Theme.high
                     : rowHover.hovered ? Theme.containerLow
                     : "transparent"

                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.right: checkMark.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Text {
                        width: parent.width
                        text: modelData.short || modelData.name
                        elide: Text.ElideRight
                        font.pixelSize: 14
                        color: Theme.onSurface
                    }
                    Text {
                        text: modelData.size + " · x" + modelData.scale
                        font.pixelSize: 11
                        font.family: Theme.mono
                        color: Theme.muted
                    }
                }

                Icon {
                    id: checkMark
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14; height: 14
                    visible: index === picker.index
                    kind: "check"
                    ink: Theme.primary
                }

                TapHandler { onTapped: { picker.picked(modelData.name); pop.close() } }
            }
        }
    }
}
