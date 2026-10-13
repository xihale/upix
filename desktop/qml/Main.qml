import QtQuick
import QtQuick.Controls.Basic

// Shell: rail | main pages | inspector, plus toast and pairing sheet.
ApplicationWindow {
    id: win
    width: 1280
    height: 840
    minimumWidth: 1024
    minimumHeight: 680
    visible: true
    color: Theme.bg
    title: {
        var j = win.snap.job
        if (j.phase === "running")
            return "超分中 " + j.percent + "% — Upix"
        if (j.phase === "done")
            return "完成 — Upix"
        if (j.phase === "error")
            return "出错 — Upix"
        return "Upix"
    }

    property string view: "stage"
    readonly property var snap: Desk.snap
    readonly property bool pairing: Desk.snap.host.pendingDevice !== "" &&
                                    Desk.snap.host.pendingFp !== ""

    // Stock controls take their colors from this palette (propagates down the
    // item tree incl. popups); re-tints on scheme flips via Theme bindings.
    palette.window: Theme.bg
    palette.windowText: Theme.onSurface
    palette.base: Theme.surface
    palette.alternateBase: Theme.container
    palette.text: Theme.onSurface
    palette.placeholderText: Theme.muted
    palette.button: Theme.high
    palette.buttonText: Theme.onSurface
    palette.highlight: Theme.primary
    palette.highlightedText: Theme.onPrimary
    palette.toolTipBase: Theme.highest
    palette.toolTipText: Theme.onSurface
    palette.link: Theme.primary
    palette.light: Theme.highest
    palette.midlight: Theme.highest
    palette.mid: Theme.high
    palette.dark: Theme.outlineV
    palette.disabled.text: Theme.muted
    palette.disabled.buttonText: Theme.muted
    palette.disabled.windowText: Theme.muted


    Component.onCompleted: bootAnim.start()

    Shortcut { sequence: "Ctrl+O"; onActivated: work.openPicker(false) }
    Shortcut { sequence: "Ctrl+Shift+O"; onActivated: work.openPicker(true) }
    Shortcut { sequences: [StandardKey.Paste]; onActivated: Desk.pasteClipboard() }
    Shortcut { sequence: "Ctrl+1"; onActivated: win.view = "stage" }
    Shortcut { sequence: "Ctrl+2"; onActivated: win.view = "models" }
    Shortcut { sequence: "Ctrl+3"; onActivated: win.view = "host" }
    Shortcut { sequence: "Ctrl+Return"; onActivated: Desk.start() }
    Shortcut { sequence: "Ctrl+."; onActivated: Desk.cancel() }
    Shortcut { sequence: "Escape"; onActivated: if (win.pairing) Desk.hostDecide(false) }

    Connections {
        target: Desk
        function onRequestView(v) { win.view = v }
    }

    // file drop
    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls) return
            var paths = []
            for (var i = 0; i < drop.urls.length; i++) paths.push(String(drop.urls[i]))
            Desk.addPaths(paths)
        }
    }

    Row {
        id: shellRow
        anchors.fill: parent

        // ---- rail ----
        // Boot-hidden: bootAnim fades these in staggered; starting at full
        // opacity made each block flash visible->invisible->fade on entry.
        Rail {
            id: rail
            opacity: 0
            y: 8
            height: parent.height
            view: win.view
            version: shortVer(win.snap.cliVersion)
            onSelect: (v) => win.view = v
        }

        Item {
            id: main
            opacity: 0
            y: 8
            width: parent.width - 76 - (win.view === "stage" ? 300 : 0)
            height: parent.height

            Workbench {
                id: work
                anchors.fill: parent
                visible: win.view === "stage"
            }
            ModelsPage {
                anchors.fill: parent
                visible: win.view === "models"
            }
            HostPage {
                anchors.fill: parent
                visible: win.view === "host"
            }
        }

        Rectangle {
            id: inspectorHost
            visible: win.view === "stage"
            opacity: 0
            y: 8
            width: visible ? 300 : 0
            height: parent.height
            color: "transparent"

            Inspector {
                anchors.fill: parent
                snap: win.snap
            }
        }
    }

    // boot rise animation (rail/main/inspector staggered 0/60/120 ms — the
    // items boot-hidden above; a pure SequentialAnimation made each block
    // pop in long after the window showed).
    ParallelAnimation {
        id: bootAnim
        SequentialAnimation {
            ParallelAnimation {
                NumberAnimation { target: rail; property: "opacity"; from: 0; to: 1; duration: 420; easing.type: Easing.Bezier; easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1] }
                NumberAnimation { target: rail; property: "y"; from: 8; to: 0; duration: 420; easing.type: Easing.Bezier; easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1] }
            }
        }
        SequentialAnimation {
            PauseAnimation { duration: 60 }
            ParallelAnimation {
                NumberAnimation { target: main; property: "opacity"; from: 0; to: 1; duration: 480; easing.type: Easing.Bezier; easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1] }
                NumberAnimation { target: main; property: "y"; from: 8; to: 0; duration: 480; easing.type: Easing.Bezier; easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1] }
            }
        }
        SequentialAnimation {
            PauseAnimation { duration: 120 }
            ParallelAnimation {
                NumberAnimation { target: inspectorHost; property: "opacity"; from: 0; to: 1; duration: 480; easing.type: Easing.Bezier; easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1] }
                NumberAnimation { target: inspectorHost; property: "y"; from: 8; to: 0; duration: 480; easing.type: Easing.Bezier; easing.bezierCurve: [0.16, 1, 0.3, 1, 1, 1] }
            }
        }
    }

    // toast. Notices carry file paths ("下载失败: /…/name"), so the pill is
    // capped and elides rather than stretching past the window.
    Rectangle {
        id: toast
        property bool shown: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: shown ? 20 : 12
        radius: 8
        color: Theme.inverse
        width: Math.min(toastText.implicitWidth + 32, parent.width - 64)
        height: toastText.implicitHeight + 20
        opacity: shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Behavior on anchors.bottomMargin { NumberAnimation { duration: 180 } }
        Text {
            id: toastText
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            text: win.snap.notice
            elide: Text.ElideMiddle
            font.pixelSize: 14
            color: Theme.onInverse
        }
        Timer {
            id: toastHide
            interval: 3200
            onTriggered: toast.shown = false
        }
    }

    Connections {
        target: Desk
        function onSnapChanged() {
            var s = Desk.snap
            if (s.noticeSeq > 0 && s.noticeSeq !== lastNoticeSeq && s.notice !== "") {
                lastNoticeSeq = s.noticeSeq
                toast.shown = true
                toastHide.restart()
            }
        }
    }
    property int lastNoticeSeq: 0

    PairingSheet { anchors.fill: parent }

    function shortVer(v) {
        if (!v) return ""
        var m = v.match(/(\d+\.\d+\.\d+)/)
        return m ? m[1] : v.slice(0, 12)
    }
}
