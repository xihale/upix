import QtQuick

// 76px navigation rail. Active entry = amber icon/text plus a 2px signal tick
// on the left edge; no sliding pill. Wordmark tops the column.
Rectangle {
    id: rail
    property string view: "stage"
    property string version: ""
    signal select(string view)

    width: 76
    color: "transparent"

    // hairline separating chrome from the work area
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.outlineV
        opacity: 0.6
    }

    Column {
        anchors.fill: parent
        anchors.margins: 12
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "upix"
            font.family: Theme.mono
            font.pixelSize: 13
            font.weight: Font.Medium
            color: Theme.onSurface
            bottomPadding: 14
        }

        Repeater {
            model: [
                { view: "stage", label: "放大", kind: "zoom-in" },
                { view: "models", label: "模型", kind: "layers" },
                { view: "host", label: "主机", kind: "smartphone" },
            ]
            delegate: Item {
                id: navItem
                required property var modelData
                readonly property bool active: rail.view === modelData.view
                width: 60
                height: 60
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    cursorShape: Qt.PointingHandCursor
                    onTapped: rail.select(navItem.modelData.view)
                }
                Rectangle {
                    visible: navItem.active
                    x: 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2
                    height: 22
                    radius: 1
                    color: Theme.primary
                }
                Column {
                    anchors.centerIn: parent
                    spacing: 3
                    Icon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 20; height: 20
                        kind: navItem.modelData.kind
                        ink: navItem.active ? Theme.primary
                            : navHover.hovered ? Theme.onSurface : Theme.muted
                        Behavior on ink { ColorAnimation { duration: 120 } }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: navItem.modelData.label
                        font.pixelSize: 12
                        color: navItem.active ? Theme.onSurface
                            : navHover.hovered ? Theme.onSurface : Theme.muted
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                }
                HoverHandler { id: navHover }
            }
        }
    }

    // Version sits on the rail's bottom edge; as a Column child it floated
    // directly under the nav items, stranded mid-rail.
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        text: rail.version
        visible: rail.version !== ""
        font.pixelSize: 10
        font.family: Theme.mono
        color: Theme.muted
    }
}
