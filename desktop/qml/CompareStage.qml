import QtQuick


// Before/after compare with draggable divider. CSS .compare: sweep animates
// pos 0.04 -> 0.62 over 720ms on first show; drag clamps 0.02..0.98.
Rectangle {
    id: stage
    property url before
    property url after
    property real pos: 0.62
    color: Theme.containerLow
    radius: 12

    // One whole device pixel, shared by the clip seam, the 1px rule and the
    // handle. Rounding them separately left the seam and the rule on adjacent
    // columns at fractional positions, which read as a doubled line mid-drag.
    readonly property int dividerPx: Math.round(width * pos)

    Image {
        anchors.fill: parent
        source: stage.before
        fillMode: Image.PreserveAspectFit
        cache: true
        autoTransform: true
    }

    Item {
        id: afterClip
        objectName: "afterClip"
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        x: stage.dividerPx
        width: Math.max(0, stage.width - stage.dividerPx)
        clip: true

        Image {
            id: afterImg
            x: -afterClip.x
            width: stage.width
            height: stage.height
            source: stage.after
            fillMode: Image.PreserveAspectFit
            cache: true
            autoTransform: true
        }
    }

    // Divider rule + handle, both anchored on the same rounded column as the
    // clip seam so the seam is never a second visible line.
    Rectangle {
        objectName: "dividerRule"
        x: stage.dividerPx
        width: 1
        height: parent.height
        color: Theme.onSurface
        opacity: 0.9
    }
    Rectangle {
        id: handle
        x: stage.dividerPx - 14
        y: parent.height / 2 - 14
        width: 28
        height: 28
        radius: 8
        color: Theme.surface
        border.color: Theme.outlineV
        border.width: 1
        Icon {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: -6
            width: 11; height: 11
            kind: "chevron-down"
            rotation: 90
            ink: Theme.muted
        }
        Icon {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: 6
            width: 11; height: 11
            kind: "chevron-down"
            rotation: -90
            ink: Theme.muted
        }
    }

    // Grab band. Declared before the chips/buttons so those stay on top and
    // keep their own clicks; a press only starts a drag within 14px of the
    // rule. The area spans the stage rather than riding on the divider: an
    // area that moves with `pos` reports `mouse.x` relative to itself, which
    // collapsed `pos` to the clamp floor on every press, and clamping `mouse.x`
    // to the band also made the divider lag the cursor on fast drags.
    MouseArea {
        id: drag
        anchors.fill: parent
        hoverEnabled: true
        // Never gate `enabled` on mouseX: the pointer leaves the band as soon
        // as the drag starts, which dropped the grab mid-gesture.
        cursorShape: pressed || Math.abs(mouseX - stage.dividerPx) <= 14
                     ? Qt.SplitHCursor : Qt.ArrowCursor
        preventStealing: true

        // Set on a press that lands on the rule; a press elsewhere must leave
        // `pos` alone even though the area swallows the whole stage.
        property bool grabbing: false

        function apply(mx) {
            sweepBg.stop()
            stage.pos = Math.min(0.98, Math.max(0.02, mx / stage.width))
        }
        onPressed: (mouse) => {
            grabbing = Math.abs(mouse.x - stage.dividerPx) <= 14
            if (grabbing) apply(mouse.x)
        }
        onReleased: grabbing = false
        onCanceled: grabbing = false
        onPositionChanged: (mouse) => { if (pressed && grabbing) apply(mouse.x) }
    }

    Rectangle {
        x: 12; y: 12
        width: chipL.implicitWidth + 16
        height: chipL.implicitHeight + 8
        radius: 6
        color: Theme.surface
        border.width: 1
        border.color: Theme.outlineV
        opacity: 0.92
        Text { id: chipL; anchors.centerIn: parent; text: "原图"; font.pixelSize: 12; color: Theme.muted }
    }
    Rectangle {
        x: parent.width - width - 12; y: 12
        width: chipR.implicitWidth + 16
        height: chipR.implicitHeight + 8
        radius: 6
        color: Theme.surface
        border.width: 1
        border.color: Theme.outlineV
        opacity: 0.92
        Text { id: chipR; anchors.centerIn: parent; text: "结果"; font.pixelSize: 12; color: Theme.muted }
    }

    // 打开 + 显示 (reveal in file manager)
    Row {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        spacing: 6
        Pill { text: "打开"; onClicked: Desk.openPath(stage.after) }
        Pill { text: "显示"; onClicked: Desk.reveal(stage.after) }
    }

    // Sweep animation on show
    SequentialAnimation {
        id: sweepBg
        running: false
        NumberAnimation {
            target: stage
            property: "pos"
            from: 0.04
            to: 0.62
            duration: 720
            easing.type: Easing.Bezier
            easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1]
        }
    }
    function sweep() {
        pos = 0.04
        sweepBg.restart()
    }
}
