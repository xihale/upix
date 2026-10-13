import QtQuick
import QtQuick.Controls.Basic


// 主机 page: header (title + host status + stop) above one pairing panel —
// QR field | hairline | instrument column with a shared label grid — and a
// quiet log strip under the panel. Idle frames the QR field with the same
// registration marks as the empty stage; amber stays on the status dot,
// the start CTA and progress only.
Rectangle {
    id: page
    color: "transparent"
    property var snap: Desk.snap
    readonly property var h: snap.host
    readonly property bool live: h.running && h.qr !== ""

    component PanelLabel: Text {
        font.pixelSize: 12
        font.weight: Font.Medium
        color: Theme.muted
    }

    // Film-carrier registration mark (same motif as the empty stage).
    component Mark: Item {
        property bool atRight: false
        property bool atBottom: false
        x: atRight ? parent.width - 18 - width : 18
        y: atBottom ? parent.height - 18 - height : 18
        width: 14
        height: 14
        rotation: atRight && atBottom ? 90
                : !atRight && atBottom ? 180
                : !atRight && !atBottom ? 270 : 0
        Rectangle {
            width: 14; height: 2; radius: 1
            anchors.top: parent.top
            anchors.right: parent.right
            color: Theme.outlineV
        }
        Rectangle {
            width: 2; height: 14; radius: 1
            anchors.top: parent.top
            anchors.right: parent.right
            color: Theme.outlineV
        }
    }

    Item {
        width: Math.min(parent.width - 64, 780)
        height: childrenRect.height
        anchors.centerIn: parent

        Column {
            width: parent.width
            spacing: 14

            // ---- header: page title, host status, stop ----
            Item {
                width: parent.width
                height: 32
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "主机"
                    font.pixelSize: 18
                    font.weight: Font.Medium
                    color: Theme.onSurface
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 6; height: 6; radius: 3
                        color: page.live ? Theme.primary : Theme.outlineV
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.live ? "运行中 · " + (h.ips.length ? h.ips[0] + ":" : "") + h.port
                            : h.running ? "启动中…"
                            : "未运行"
                        font.pixelSize: 12
                        font.family: Theme.mono
                        color: Theme.muted
                    }
                    Pill {
                        visible: h.running
                        anchors.verticalCenter: parent.verticalCenter
                        compact: true
                        flat: true
                        icon: "x"
                        text: "停止主机"
                        onClicked: Desk.hostStop()
                    }
                }
            }

            // ---- pairing panel ----
            Rectangle {
                id: panel
                width: parent.width
                height: Math.max(336, body.height + 56)
                radius: 12
                color: Theme.containerLow
                border.width: 1
                border.color: Theme.outlineV

                Row {
                    id: body
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 28

                    // QR field. Scanners need a white ground once live.
                    Item {
                        width: 280
                        height: 280

                        Rectangle {
                            visible: page.live
                            anchors.fill: parent
                            radius: 8
                            color: "#ffffff"
                            border.width: page.live && !Theme.dark ? 1 : 0
                            border.color: Theme.outlineV
                        }
                        Image {
                            visible: page.live
                            anchors.fill: parent
                            anchors.margins: 16
                            source: page.live ? "image://qr/" + encodeURIComponent(h.qr) : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: false
                            cache: false
                        }

                        Mark { visible: !page.live }
                        Mark { visible: !page.live; atRight: true }
                        Mark { visible: !page.live; atBottom: true }
                        Mark { visible: !page.live; atRight: true; atBottom: true }

                        Column {
                            visible: !page.live
                            anchors.centerIn: parent
                            width: 224
                            spacing: 12

                            Icon {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 40; height: 40
                                kind: "qr-code"
                                ink: Theme.muted
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: snap.cliFound ? "在手机上扫码配对" : "未找到 upix-cli"
                                font.pixelSize: 15
                                font.weight: Font.Medium
                                color: Theme.onSurface
                            }
                            Text {
                                width: parent.width
                                visible: snap.cliFound
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                text: "同一 Wi-Fi 下的手机扫码，即可把照片发到这台电脑。"
                                font.pixelSize: 12
                                color: Theme.muted
                            }
                        }
                    }

                    // hairline between the field and the instrument column
                    Rectangle {
                        width: 1
                        height: parent.height
                        color: Theme.outlineV
                        opacity: 0.7
                    }

                    // instrument column: every value hangs on the x=56 grid
                    Column {
                        id: det
                        width: panel.width - 56 - 280 - 1 - 56
                        spacing: 12

                        // 指纹 — click the copy button
                        Item {
                            width: parent.width
                            height: 48
                            PanelLabel {
                                x: 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: "指纹"
                            }
                            Column {
                                x: 56
                                width: parent.width - 56
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3
                                Row {
                                    spacing: 8
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: h.fingerprint !== "" ? fpSpaced(h.fingerprint) : "—"
                                        font.pixelSize: 13
                                        font.family: Theme.mono
                                        color: h.fingerprint !== "" ? Theme.onSurface : Theme.muted
                                    }
                                    Item {
                                        id: copyBtn
                                        visible: h.fingerprint !== ""
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 24; height: 24
                                        HoverHandler { id: copyHover; cursorShape: Qt.PointingHandCursor }
                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 8
                                            color: copyHover.hovered ? Theme.high : "transparent"
                                        }
                                        Icon {
                                            anchors.centerIn: parent
                                            width: 13; height: 13
                                            kind: "copy"
                                            ink: copyHover.hovered ? Theme.onSurface : Theme.muted
                                        }
                                        ToolTip.visible: copyHover.hovered
                                        ToolTip.text: "复制指纹"
                                        ToolTip.delay: 400
                                        TapHandler { onTapped: Desk.copyFingerprint() }
                                    }
                                }
                                Text {
                                    text: "两端的指纹一致才安全"
                                    font.pixelSize: 11
                                    color: Theme.muted
                                    opacity: 0.8
                                }
                            }
                        }

                        Rectangle { width: parent.width; height: 1; color: Theme.outlineV; opacity: 0.6 }

                        // 地址 — first card is the QR's default
                        Item {
                            width: parent.width
                            height: Math.max(1, h.ips.length) * 26
                            PanelLabel {
                                x: 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: "地址"
                            }
                            Column {
                                x: 56
                                width: parent.width - 56
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    visible: h.ips.length === 0
                                    height: 26
                                    verticalAlignment: Text.AlignVCenter
                                    text: "—"
                                    font.pixelSize: 13
                                    font.family: Theme.mono
                                    color: Theme.muted
                                }
                                Repeater {
                                    model: h.ips
                                    delegate: Item {
                                        required property string modelData
                                        required property int index
                                        width: parent ? parent.width : 0
                                        height: 26
                                        Text {
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: modelData
                                            font.pixelSize: 13
                                            font.family: Theme.mono
                                            color: index === 0 ? Theme.onSurface : Theme.muted
                                        }
                                        Rectangle {
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: index === 0
                                            width: defText.implicitWidth + 12
                                            height: 16
                                            radius: 4
                                            color: "transparent"
                                            border.width: 1
                                            border.color: Theme.outlineV
                                            Text {
                                                id: defText
                                                anchors.centerIn: parent
                                                text: "默认"
                                                font.pixelSize: 10
                                                color: Theme.muted
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle { width: parent.width; height: 1; color: Theme.outlineV; opacity: 0.6 }

                        // 端口
                        Item {
                            width: parent.width
                            height: 32
                            PanelLabel {
                                x: 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: "端口"
                            }
                            Item {
                                x: 56
                                anchors.verticalCenter: parent.verticalCenter
                                width: 96
                                height: 32
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 8
                                    color: "transparent"
                                    border.width: 1
                                    border.color: portField.activeFocus ? Theme.primary : Theme.outlineV
                                }
                                TextInput {
                                    id: portField
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    verticalAlignment: TextInput.AlignVCenter
                                    text: h.port
                                    readOnly: h.running
                                    font.pixelSize: 13
                                    font.family: Theme.mono
                                    color: Theme.onSurface
                                    selectByMouse: true
                                    validator: IntValidator { bottom: 1; top: 65535 }
                                    onEditingFinished: Desk.setHostPort(parseInt(text) || 0)
                                }
                            }
                        }

                        Rectangle { width: parent.width; height: 1; color: Theme.outlineV; opacity: 0.6 }

                        // start control swaps for task progress once live
                        Pill {
                            visible: !h.running
                            enabled: snap.cliFound
                            accent: true
                            icon: "play"
                            text: "启动主机"
                            onClicked: Desk.hostStart()
                        }
                        Column {
                            visible: h.running && h.taskPercent >= 0
                            width: parent.width
                            spacing: 4
                            Item {
                                width: parent.width
                                height: 18
                                PanelLabel {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "手机任务"
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: Math.max(0, h.taskPercent) + "%"
                                    font.pixelSize: 13
                                    font.family: Theme.mono
                                    color: Theme.onSurface
                                }
                            }
                            Progress {
                                width: parent.width
                                from: 0
                                to: 100
                                value: Math.max(0, h.taskPercent)
                            }
                        }
                    }
                }
            }

            // log — a quiet terminal strip under the panel
            Rectangle {
                visible: h.running && h.log.length > 0
                width: parent.width
                height: Math.min(112, logText.implicitHeight + 20)
                radius: 8
                color: Theme.containerLow
                border.width: 1
                border.color: Theme.outlineV
                clip: true
                Text {
                    id: logText
                    anchors.fill: parent
                    anchors.margins: 10
                    text: h.log.slice(-6).join("\n")
                    font.pixelSize: 11
                    font.family: Theme.mono
                    color: Theme.muted
                    wrapMode: Text.Wrap
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
