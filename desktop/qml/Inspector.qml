import QtQuick
import QtQuick.Controls.Basic

// Right inspector: model picker, output format, quality, processing toggles,
// CTA + daemon row. All controls are the hand-drawn token components
// (Picker/Segmented/TrackSlider/Toggle/StepRow/Pill) — stock Controls.Basic
// widgets render library chrome (Basic-style popups, switch colors, spinner
// frames, palette-derived progress fill) that cannot take the container/
// radius/tone language.
Rectangle {
    id: inspector
    color: "transparent"
    property var snap: Desk.snap
    readonly property var job: snap.job
    readonly property bool folder: snap.inputs.some(function (i) { return i.dir })
    readonly property bool canRun: snap.cliFound && snap.inputs.length > 0 &&
                                   snap.model !== "" && job.phase !== "running"

    component SectionLabel: Text {
        font.pixelSize: 12
        font.weight: Font.Medium
        color: Theme.muted
    }

    Column {
        anchors.fill: parent
        anchors.topMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 16
        anchors.leftMargin: 8
        spacing: 12

        Flickable {
            id: scroll
            width: parent.width
            height: parent.height - dock.height - 12
            contentHeight: body.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            Column {
                id: body
                width: scroll.width - 2
                spacing: 20

                // CLI missing
                Column {
                    visible: !snap.cliFound
                    spacing: 8
                    Text { text: "未找到 upix-cli"; font.pixelSize: 14; color: Theme.error }
                    Text { text: "设置 UPIX_CLI,或把 upix-cli 放到 PATH。"; font.pixelSize: 12; color: Theme.muted }
                }

                // ---- 模型 ----
                Column {
                    width: parent.width
                    spacing: 8
                    SectionLabel { text: "模型" }
                    Picker {
                        width: parent.width
                        items: snap.installed
                        current: snap.model
                        onPicked: (name) => Desk.setModel(name)
                    }
                }

                // ---- 输出 ----
                Column {
                    width: parent.width
                    spacing: 12
                    SectionLabel { text: "输出" }
                    Segmented {
                        width: parent.width
                        items: [
                            { label: "跟随", value: "" },
                            { label: "PNG", value: "png" },
                            { label: "JPG", value: "jpg" },
                            { label: "WebP", value: "webp" },
                            { label: "AVIF", value: "avif" },
                        ]
                        // Without this the indicator sat on 跟随 forever and
                        // every pick read as "只能选择跟随".
                        current: snap.format
                        onPicked: (v) => Desk.setFormat(v)
                    }
                    Column {
                        width: parent.width
                        spacing: 4
                        Item {
                            width: parent.width
                            height: 20
                            Text { text: "质量"; font.pixelSize: 14; color: Theme.onSurface; anchors.verticalCenter: parent.verticalCenter }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: snap.quality
                                font.pixelSize: 14
                                font.family: Theme.mono
                                color: Theme.onSurface
                            }
                        }
                        TrackSlider {
                            width: parent.width
                            from: 1
                            to: 100
                            value: snap.quality
                            onMoved: (v) => Desk.setQuality(Math.round(v))
                        }
                    }
                }

                // ---- 处理 ----
                Column {
                    width: parent.width
                    spacing: 8
                    SectionLabel { text: "处理"; bottomPadding: 4 }
                    Toggle {
                        width: parent.width
                        label: "文件夹用 GPU"
                        checked: snap.gpu
                        onToggled: (v) => Desk.setGpu(v)
                    }
                    Text {
                        visible: inspector.folder && snap.gpu
                        width: parent.width
                        text: "文件夹逐张用 GPU,不并行。"
                        font.pixelSize: 12
                        color: Theme.muted
                    }
                    Text {
                        visible: !inspector.folder
                        width: parent.width
                        text: "单张交给热引擎跑,不可用时回退独立进程。"
                        font.pixelSize: 12
                        color: Theme.muted
                    }
                    StepRow {
                        visible: inspector.folder && !snap.gpu
                        width: parent.width
                        value: snap.jobs
                        onChanged: (v) => Desk.setJobs(Math.max(1, Math.min(8, v)))
                    }
                    Toggle {
                        width: parent.width
                        label: "自动分块"
                        checked: snap.autoTile
                        onToggled: (v) => Desk.setAutoTile(v)
                    }
                    Column {
                        visible: !snap.autoTile
                        width: parent.width
                        spacing: 4
                        Item {
                            width: parent.width
                            height: 20
                            Text { text: "分块"; font.pixelSize: 14; color: Theme.onSurface; anchors.verticalCenter: parent.verticalCenter }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: snap.tileSize
                                font.pixelSize: 14
                                font.family: Theme.mono
                                color: Theme.onSurface
                            }
                        }
                        TrackSlider {
                            width: parent.width
                            from: 64
                            to: 1024
                            step: 32
                            value: snap.tileSize
                            onMoved: (v) => Desk.setTileSize(v)
                        }
                    }
                    Toggle {
                        width: parent.width
                        label: "重新处理已放大的图"
                        checked: snap.force
                        onToggled: (v) => Desk.setForce(v)
                    }
                }
            }
        }

        // ---- dock ----
        Column {
            id: dock
            width: parent.width
            spacing: 8

            // CTA: filled primary when runnable, outline-error while running,
            // quiet container when disabled (a dimmed primary still read as
            // enabled at 0.55 opacity).
            Item {
                id: cta
                width: parent.width
                height: 44
                readonly property bool running: job.phase === "running"
                readonly property bool live: running || inspector.canRun
                readonly property color fill: running ? "transparent"
                    : inspector.canRun ? Theme.primary : Theme.high
                readonly property color ink: running ? Theme.error
                    : inspector.canRun ? Theme.onPrimary : Theme.muted
                HoverHandler { cursorShape: cta.live ? Qt.PointingHandCursor : Qt.ArrowCursor }
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: cta.fill
                    border.width: 1
                    border.color: cta.running ? Theme.error : "transparent"
                }
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !cta.running
                        width: 16; height: 16
                        kind: "play"
                        fill: true
                        ink: cta.ink
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: cta.running ? "停止" : "开始超分"
                        font.pixelSize: 15
                        font.weight: Font.Medium
                        color: cta.ink
                    }
                }
                TapHandler {
                    enabled: cta.live
                    onTapped: cta.running ? Desk.cancel() : Desk.start()
                }
            }

            // daemon row. The raw CLI line carries a 40+ char socket path and
            // overran the 300px inspector; show a compact status and keep the
            // verbatim line in the tooltip.
            Item {
                width: parent.width
                height: 32
                readonly property var daemon: snap.daemon
                readonly property string stateText: {
                    if (!daemon.running) return "未运行"
                    if (daemon.socketMissing) return "运行中 · socket 丢失"
                    if (daemon.pid > 0) return "运行中 · pid " + daemon.pid
                    return "运行中"
                }
                HoverHandler { id: daemonHover }
                Row {
                    anchors.left: parent.left
                    anchors.right: pillStop.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Text {
                        id: hotLabel
                        anchors.verticalCenter: parent.verticalCenter
                        text: "热引擎"
                        font.pixelSize: 12
                        color: Theme.muted
                    }
                    Text {
                        // baseline (not verticalCenter): mono and sans at the
                        // same pixel size center unevenly and read as wobble.
                        anchors.baseline: hotLabel.baseline
                        width: Math.max(0, parent.width - parent.spacing - 48)
                        text: parent.parent.stateText
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        font.family: Theme.mono
                        color: snap.daemon.socketMissing ? Theme.error : Theme.muted
                    }
                }
                ToolTip.visible: daemonHover.hovered && snap.daemon.label !== ""
                ToolTip.delay: 400
                ToolTip.text: snap.daemon.label
                Pill {
                    id: pillStop
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    flat: true
                    enabled: snap.cliFound
                    text: snap.daemon.running ? "停止" : "启动"
                    onClicked: Desk.daemonAction(snap.daemon.running ? "stop" : "start")
                }
            }
        }
    }
}
