import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// Stage page: empty zones / single image / folder cover / compare, plus
// running veil, error banner, note, thumb strip.
Rectangle {
    id: work
    color: "transparent"
    property var snap: Desk.snap
    readonly property var job: snap.job
    readonly property bool isDir: snap.inputs.length > 0 && snap.inputs[0].dir
    readonly property bool one: snap.inputs.length === 1 && !snap.inputs[0].dir
    readonly property bool compare: one && job.phase === "done" && job.outputs.length >= 1 &&
                                    String(job.message).indexOf("跳过") === -1
    readonly property url lastOutput: compare ? job.outputs[job.outputs.length - 1] : ""

    function srcFor() {
        if (snap.inputs.length === 0) return ""
        if (isDir) return snap.cover
        if (job.phase === "done" && job.outputs.length) return job.outputs[job.outputs.length - 1]
        var idx = job.phase === "running" ? Math.max(0, job.index - 1) : 0
        var inp = snap.inputs[idx] || snap.inputs[0]
        return inp ? inp.url : ""
    }

    Column {
        anchors.fill: parent
        anchors.topMargin: 16
        anchors.rightMargin: 8
        anchors.bottomMargin: 16
        spacing: 12

        // ---- stage ----
        Rectangle {
            id: stageBox
            width: parent.width
            height: parent.height - (strip.visible ? strip.height + 12 : 0)
            radius: 12
            color: Theme.containerLow
            clip: true

            // Empty: film-carrier framing. Corner registration marks delimit
            // the drop field; tap opens the image picker, the folder action
            // has its own quiet text button.
            Item {
                anchors.fill: parent
                visible: snap.inputs.length === 0

                HoverHandler { id: zoneHover; enabled: zoneTap.enabled }
                TapHandler {
                    id: zoneTap
                    onTapped: picker.openFor(false)
                }
                Rectangle {
                    anchors.fill: parent
                    color: Theme.high
                    opacity: zoneHover.hovered ? 0.45 : 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                }

                // Registration marks: an L hugging each corner of the frame,
                // reading as the film carrier. One L + rotation keeps all
                // four legs consistent.
                component Mark: Item {
                    property bool atRight: false
                    property bool atBottom: false
                    x: atRight ? parent.width - 32 - width : 32
                    y: atBottom ? parent.height - 32 - height : 32
                    width: 18
                    height: 18
                    // base L hugs this item's top-right corner
                    rotation: atRight && atBottom ? 90
                            : !atRight && atBottom ? 180
                            : !atRight && !atBottom ? 270 : 0
                    Rectangle {
                        width: 18; height: 2; radius: 1
                        anchors.top: parent.top
                        anchors.right: parent.right
                        color: zoneHover.hovered ? Theme.muted : Theme.outlineV
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Rectangle {
                        width: 2; height: 18; radius: 1
                        anchors.top: parent.top
                        anchors.right: parent.right
                        color: zoneHover.hovered ? Theme.muted : Theme.outlineV
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                }
                Mark { }
                Mark { atRight: true }
                Mark { atBottom: true }
                Mark { atRight: true; atBottom: true }

                Column {
                    anchors.centerIn: parent
                    spacing: 14

                    Icon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 34; height: 34
                        kind: "zoom-in"
                        ink: zoneHover.hovered ? Theme.onSurface : Theme.muted
                        Behavior on ink { ColorAnimation { duration: 120 } }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "拖入图片，或按 Ctrl+V 粘贴"
                        font.pixelSize: 15
                        color: zoneHover.hovered ? Theme.onSurface : Theme.muted
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                }

                // Folder action sits inside the frame's bottom edge so both
                // entry points stay one glance apart.
                Item {
                    id: folderAction
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 44
                    width: folderRow.implicitWidth + 24
                    height: 32
                    HoverHandler { id: folderHover; cursorShape: Qt.PointingHandCursor }
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: folderHover.hovered ? Theme.high : "transparent"
                        border.width: 1
                        border.color: Theme.outlineV
                    }
                    Row {
                        id: folderRow
                        anchors.centerIn: parent
                        spacing: 7
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14
                            kind: "folder-open"
                            ink: folderHover.hovered ? Theme.onSurface : Theme.muted
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "选择文件夹批量处理"
                            font.pixelSize: 13
                            color: folderHover.hovered ? Theme.onSurface : Theme.muted
                        }
                    }
                    TapHandler { onTapped: picker.openFor(true) }
                }
            }

            // Compare mode
            CompareStage {
                anchors.fill: parent
                visible: work.compare
                before: snap.inputs.length ? snap.inputs[0].url : ""
                after: work.lastOutput
                onVisibleChanged: if (visible) sweep()
            }

            // Single image / folder cover / latest output
            Image {
                anchors.fill: parent
                visible: !work.compare && snap.inputs.length > 0
                source: work.srcFor()
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                autoTransform: true
            }

            // Clear button: small etched square, ✕ glyph from the icon set.
            Item {
                id: clearBtn
                visible: !work.compare && snap.inputs.length > 0 && job.phase !== "running"
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 12
                width: 26
                height: 26
                HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: clearHover.hovered ? Theme.high : Theme.containerLow
                    border.width: 1
                    border.color: Theme.outlineV
                }
                Icon {
                    anchors.centerIn: parent
                    width: 12; height: 12
                    kind: "x"
                    ink: clearHover.hovered ? Theme.onSurface : Theme.muted
                }
                ToolTip.visible: clearHover.hovered
                ToolTip.text: "清空列表"
                ToolTip.delay: 400
                TapHandler { onTapped: Desk.clearInputs() }
            }

            // Folder badge
            Rectangle {
                visible: work.isDir
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 12
                radius: 999
                color: Theme.highest
                width: badgeText.implicitWidth + 20
                height: badgeText.implicitHeight + 8
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: "文件夹" + (snap.folderCount ? " · " + snap.folderCount + " 张" : "")
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    color: Theme.onSurface
                }
            }

            // Running veil
            Rectangle {
                visible: job.phase === "running"
                anchors.fill: parent
                color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0.55)

                Text {
                    id: pctText
                    x: 16
                    anchors.bottom: track.top
                    anchors.bottomMargin: 10
                    text: job.percent + "%"
                    font.pixelSize: 22
                    font.family: Theme.mono
                    color: Theme.onSurface
                }
                Text {
                    anchors.baseline: pctText.baseline
                    anchors.left: pctText.right
                    anchors.leftMargin: 12
                    // Bound to the stage: `job.current` is a filename and the
                    // row was unbounded, so a long name ran off the right edge.
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    text: {
                        var bits = []
                        if (job.total > 1) bits.push(job.index + "/" + job.total)
                        if (job.tilesTotal > 0) bits.push(job.tilesDone + "/" + job.tilesTotal)
                        if (job.etaSec > 0) bits.push(job.etaSec + "s")
                        if (job.current && snap.inputs.length > 1) bits.push(job.current)
                        return bits.join(" · ")
                    }
                    elide: Text.ElideMiddle
                    font.pixelSize: 12
                    color: Theme.muted
                }
                Progress {
                    id: track
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    from: 0
                    to: 100
                    value: job.percent
                }
            }

            // Done note. `job.message` can carry a long CLI tail (a path or a
            // whole stderr line) and the chip was sized to its content, so it
            // grew past the stage and under the 打开/显示 buttons. Cap at half
            // the stage (the right half belongs to those buttons) and elide;
            // the full text stays in the tooltip.
            Rectangle {
                id: noteChip
                visible: job.phase === "done" && job.message
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 12
                radius: 8
                color: Theme.containerLow
                border.width: 1
                border.color: Theme.outlineV
                width: Math.min(noteText.implicitWidth + 24, parent.width / 2)
                height: noteText.implicitHeight + 14
                HoverHandler { id: noteHover }
                Text {
                    id: noteText
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    verticalAlignment: Text.AlignVCenter
                    text: job.message
                    elide: Text.ElideRight
                    font.pixelSize: 14
                    color: Theme.onSurface
                }
                ToolTip.visible: noteHover.hovered && noteText.truncated
                ToolTip.delay: 400
                ToolTip.text: job.message
            }

            // Open + reveal (non-compare)
            Row {
                visible: !work.compare && job.outputs.length > 0
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                spacing: 6
                Pill { text: "打开"; onClicked: Desk.openPath(work.srcFor()) }
                Pill { text: "显示"; onClicked: Desk.reveal("") }
            }

            // Error banner
            Rectangle {
                visible: job.phase === "error" && job.message
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                radius: 8
                color: Theme.errorC
                border.width: 1
                border.color: Theme.error
                height: errText.implicitHeight + 20
                Text {
                    id: errText
                    anchors.fill: parent
                    anchors.margins: 10
                    text: job.message
                    font.pixelSize: 14
                    wrapMode: Text.Wrap
                    color: Theme.onErrorC
                }
            }
        }

        // ---- thumb strip (multi-file) ----
        Row {
            id: strip
            visible: snap.inputs.length > 1 && !work.isDir
            spacing: 8
            height: visible ? 72 : 0

            Repeater {
                model: snap.inputs

                delegate: Rectangle {
                    required property var modelData
                    readonly property bool current: job.phase === "running" &&
                        snap.inputs[Math.max(0, job.index - 1)] &&
                        modelData.path === snap.inputs[Math.max(0, job.index - 1)].path
                    width: 72; height: 72; radius: 8
                    color: Theme.container
                    clip: true
                    border.color: current ? Theme.primary : Theme.outlineV
                    border.width: current ? 2 : 1

                    Image {
                        anchors.fill: parent
                        source: modelData.url
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        autoTransform: true
                    }
                    // delete circle over the thumbnail
                    Item {
                        x: parent.width - width - 4
                        y: 4
                        width: 22; height: 22
                        HoverHandler { id: delHover; cursorShape: Qt.PointingHandCursor }
                        Rectangle {
                            anchors.fill: parent
                            radius: 11
                            color: delHover.hovered ? Theme.onSurface : Theme.highest
                            border.color: Theme.outlineV
                            border.width: 1
                        }
                        Icon {
                            anchors.centerIn: parent
                            width: 11; height: 11
                            kind: "x"
                            ink: delHover.hovered ? Theme.highest : Theme.onSurface
                        }
                        TapHandler { onTapped: Desk.removeInput(modelData.path) }
                    }
                }
            }

            // add tile: dashed-outline well + plus glyph
            Item {
                id: addTile
                width: 72; height: 72
                HoverHandler { id: addHover; cursorShape: Qt.PointingHandCursor }
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: addHover.hovered ? Theme.high : "transparent"
                    border.color: Theme.outlineV
                    border.width: 1
                }
                Icon {
                    anchors.centerIn: parent
                    width: 18; height: 18
                    kind: "plus"
                    ink: addHover.hovered ? Theme.onSurface : Theme.muted
                }
                ToolTip.visible: addHover.hovered
                ToolTip.text: "添加图片"
                ToolTip.delay: 400
                TapHandler { onTapped: picker.openFor(false) }
            }
        }
    }

    function openPicker(dir) { picker.openFor(dir) }

    FileDialog {
        id: picker
        property bool forDir: false
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.avif *.bmp *.tif *.tiff)", "全部文件 (*)"]
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            var paths = []
            for (var i = 0; i < selectedFiles.length; i++) paths.push(String(selectedFiles[i]))
            Desk.addPaths(paths)
        }
        function openFor(dir) { forDir = dir; if (dir) folderPicker.open(); else open() }
    }

    FolderDialog {
        id: folderPicker
        onAccepted: Desk.addPaths([String(selectedFolder)])
    }
}
