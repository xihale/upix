import QtQuick

// Button. Instrument grammar: 8px radius, 1px etched outline for resting
// buttons, hover lifts one step (high); `flat` is a borderless text action;
// `accent` is the single primary-filled action. `icon` is an Icon.kind.
Item {
    id: pill
    property string text: ""
    property string icon: ""
    property bool accent: false
    property bool flat: false
    property bool compact: false
    signal clicked()

    implicitWidth: content.implicitWidth + (flat ? 16 : compact ? 18 : 24)
    implicitHeight: compact ? 28 : 34
    opacity: enabled ? 1 : 0.4

    // Every button in the app is this component: it carries the keyboard path
    // (Tab + Space/Return) and the screen-reader name for all of them.
    activeFocusOnTab: enabled
    Accessible.role: Accessible.Button
    Accessible.name: pill.text
    Accessible.onPressAction: if (pill.enabled) pill.clicked()

    HoverHandler { id: hover; cursorShape: pill.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: pill.accent ? Theme.primary
             : hover.hovered && pill.enabled ? Theme.high
             : "transparent"
        // Focus ring follows the HostPage port field grammar (primary hairline);
        // on an accent-filled button it has to be onPrimary to stay visible.
        border.width: pill.accent || pill.flat ? (pill.activeFocus ? 1 : 0) : 1
        border.color: pill.activeFocus ? (pill.accent ? Theme.onPrimary : Theme.primary)
                                       : Theme.outlineV
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 7
        Icon {
            visible: pill.icon !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: 15; height: 15
            kind: pill.icon
            ink: pill.accent ? Theme.onPrimary
                : pill.flat && !hover.hovered ? Theme.muted
                : Theme.onSurface
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: pill.text
            font.pixelSize: pill.compact ? 12 : 13
            font.weight: Font.Medium
            color: pill.accent ? Theme.onPrimary
                 : pill.flat && !hover.hovered ? Theme.muted
                 : Theme.onSurface
        }
    }

    TapHandler { enabled: pill.enabled; onTapped: pill.clicked() }

    Keys.onReturnPressed: if (pill.enabled) pill.clicked()
    Keys.onEnterPressed: if (pill.enabled) pill.clicked()
    Keys.onSpacePressed: if (pill.enabled) pill.clicked()
}
