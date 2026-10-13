import QtQuick


// 配对请求 sheet over a scrim: device glyph, name, and the fingerprint both
// sides must agree on. pop 240ms.
Rectangle {
    id: scrim
    anchors.fill: parent
    color: Qt.rgba(0, 0, 0, 0.55)
    visible: Desk.snap.host.pendingDevice !== "" && Desk.snap.host.pendingFp !== ""
    onVisibleChanged: if (visible) { sheet.scale = 0.98; sheet.opacity = 0; popAnim.restart() }

    Rectangle {
        id: sheet
        width: 400
        height: content.implicitHeight + 44
        radius: 12
        color: Theme.surface
        border.width: 1
        border.color: Theme.outlineV
        anchors.centerIn: parent

        SequentialAnimation {
            id: popAnim
            ParallelAnimation {
                NumberAnimation { target: sheet; property: "opacity"; to: 1; duration: 240; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] }
                NumberAnimation { target: sheet; property: "scale"; to: 1; duration: 240; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] }
            }
        }
        transformOrigin: Item.Center

        Column {
            id: content
            anchors.fill: parent
            anchors.margins: 22
            spacing: 6

            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 30; height: 36
                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    width: 30; height: 30
                    kind: "smartphone"
                    ink: Theme.primary
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                text: Desk.snap.host.pendingDevice
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 17
                font.weight: Font.Medium
                color: Theme.onSurface
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "请求连接到这台电脑"
                font.pixelSize: 12
                color: Theme.muted
                bottomPadding: 8
            }

            // verification field: the shared secret both screens show
            Rectangle {
                width: parent.width
                height: fpText.implicitHeight + 40
                radius: 8
                color: Theme.containerLow
                border.width: 1
                border.color: Theme.outlineV
                Column {
                    anchors.centerIn: parent
                    spacing: 4
                    Text {
                        id: fpLabel
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "设备指纹"
                        font.pixelSize: 11
                        color: Theme.muted
                    }
                    Text {
                        id: fpText
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: fpSpaced(Desk.snap.host.pendingFp)
                        font.pixelSize: 16
                        font.family: Theme.mono
                        font.letterSpacing: 1
                        color: Theme.onSurface
                    }
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "与手机上显示的指纹核对一致后，再点允许。"
                font.pixelSize: 11
                color: Theme.muted
                bottomPadding: 12
            }

            Row {
                width: parent.width
                spacing: 8

                Item {
                    id: allowBtn
                    width: parent.width - denyBtn.width - 8
                    height: 40
                    HoverHandler { id: allowHover; cursorShape: Qt.PointingHandCursor }
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: allowHover.hovered ? Theme.primary : Qt.darker(Theme.primary, 1.08)
                    }
                    Row {
                        anchors.centerIn: parent
                        spacing: 8
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14
                            kind: "check"
                            ink: Theme.onPrimary
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "允许连接"
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: Theme.onPrimary
                        }
                    }
                    TapHandler { onTapped: Desk.hostDecide(true) }
                }
                Pill {
                    id: denyBtn
                    height: 40
                    icon: "x"
                    text: "拒绝 (Esc)"
                    onClicked: Desk.hostDecide(false)
                }
            }
        }
    }

    function fpSpaced(fp) {
        var out = ""
        for (var i = 0; i < fp.length; i += 4) {
            if (i) out += " "
            out += fp.slice(i, i + 4)
        }
        return out
    }
}
