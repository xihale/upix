import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import "Grouping.js" as G

// 模型 page: installed catalog grouped by family + downloadable list.
Flickable {
    id: page
    contentWidth: width
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    property var snap: Desk.snap

    contentHeight: inner.height

    ScrollBar.vertical: ScrollBar { }

    Rectangle {
        id: inner
        y: 28
        width: Math.min(parent.width - 64, 840)
        x: (page.width - width) / 2
        height: childrenRect.height + 40
        color: "transparent"

        Column {
            width: parent.width
            spacing: 8

            // head
            Item {
                width: parent.width
                height: 32
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "模型"
                    font.pixelSize: 18
                    font.weight: Font.Medium
                    color: Theme.onSurface
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Pill {
                        compact: true
                        flat: true
                        text: "打开目录"
                        enabled: snap.modelsDir !== ""
                        onClicked: Desk.reveal(snap.modelsDir)
                    }
                    Pill {
                        compact: true
                        flat: true
                        text: "导入"
                        onClicked: importDialog.open()
                    }
                    Pill {
                        compact: true
                        flat: true
                        icon: "refresh-cw"
                        text: "刷新"
                        onClicked: Desk.refreshModels()
                    }
                }
            }

            Text {
                width: parent.width
                text: snap.modelsDir
                font.pixelSize: 12
                font.family: Theme.mono
                color: Theme.muted
                elide: Text.ElideMiddle
                bottomPadding: 12
            }

            // 已安装
            Text {
                text: "已安装"
                font.pixelSize: 12
                font.weight: Font.Medium
                color: Theme.muted
            }
            Text { visible: snap.installed.length === 0; text: "未发现模型"; font.pixelSize: 12; color: Theme.muted }
            Column {
                width: parent.width
                spacing: 2
                Repeater {
                    model: G.groups(snap.installed)
                    delegate: Column {
                        required property var modelData
                        width: parent.width
                        Text {
                            visible: modelData.label !== ""
                            text: modelData.label
                            font.pixelSize: 12
                            color: Theme.muted
                            leftPadding: 8
                            topPadding: 8
                            bottomPadding: 2
                        }
                        Repeater {
                            model: modelData.items
                            delegate: CatalogRow {
                                width: parent ? parent.width : 0
                                entry: modelData.m
                                shortName: modelData.short
                                isRemote: false
                                current: snap.model === modelData.m.name
                            }
                        }
                    }
                }
            }

            // 可下载
            Text {
                text: "可下载"
                font.pixelSize: 12
                font.weight: Font.Medium
                color: Theme.muted
                topPadding: 16
            }
            Text { visible: snap.remoteError !== ""; text: snap.remoteError; font.pixelSize: 14; color: Theme.error }
            Text {
                visible: !snap.remoteError && remoteFiltered().length === 0
                text: snap.remote.length ? "都已在本地" : "正在获取"
                font.pixelSize: 12
                color: Theme.muted
            }
            Column {
                width: parent.width
                spacing: 2
                Repeater {
                    model: remoteFiltered()
                    delegate: CatalogRow {
                        width: parent ? parent.width : 0
                        entry: modelData
                        shortName: modelData.name
                        isRemote: true
                        current: false
                    }
                }
            }
        }
    }

    function remoteFiltered() {
        var names = {}
        for (var i = 0; i < snap.installed.length; i++) names[snap.installed[i].name] = true
        return snap.remote.filter(function (m) { return !names[m.name] })
    }

    FileDialog {
        id: importDialog
        nameFilters: ["ncnn 模型 (*.param *.bin)"]
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            var paths = []
            for (var i = 0; i < selectedFiles.length; i++) paths.push(String(selectedFiles[i]))
            Desk.importModels(paths)
        }
    }

    component CatalogRow: Rectangle {
        id: row
        property var entry
        property string shortName
        property bool isRemote
        property bool current
        height: 40
        radius: 6
        color: current ? Theme.high : (hover.hovered ? Theme.containerLow : "transparent")

        // amber signal tick marks the active model
        Rectangle {
            visible: row.current
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            width: 2
            height: 18
            radius: 1
            color: Theme.primary
        }

        readonly property bool downloading: isRemote && snap.downloading === entry.name
        HoverHandler { id: hover; cursorShape: row.isRemote ? Qt.ArrowCursor : Qt.PointingHandCursor }

        Row {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 8
            spacing: 8

            Text {
                width: Math.max(0, parent.width - badges.width - sizeText.width
                                  - actionLoader.width - 32)
                anchors.verticalCenter: parent.verticalCenter
                text: shortName || entry.name
                elide: Text.ElideRight
                font.pixelSize: 14
                color: Theme.onSurface
            }
            Row {
                id: badges
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // 适用场景 chip (动漫/照片/综合/修复…), same grammar as the
                // "默认" badge in HostPage. Catalog models only — imported
                // models carry no scene and get no chip.
                Rectangle {
                    visible: (entry.scene || "") !== ""
                    width: sceneLabel.implicitWidth + 12
                    height: 16
                    radius: 4
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.outlineV
                    Text {
                        id: sceneLabel
                        anchors.centerIn: parent
                        text: entry.scene || ""
                        font.pixelSize: 10
                        color: Theme.muted
                    }
                }
                // ★ the picks models/README.md calls out as first choices;
                // amber = the theme's single signal color.
                Rectangle {
                    visible: entry.recommended === true
                    width: recoLabel.implicitWidth + 12
                    height: 16
                    radius: 4
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.primary
                    Text {
                        id: recoLabel
                        anchors.centerIn: parent
                        text: "推荐"
                        font.pixelSize: 10
                        color: Theme.primary
                    }
                }
            }
            Text {
                id: sizeText
                anchors.verticalCenter: parent.verticalCenter
                text: entry.size
                font.pixelSize: 12
                font.family: Theme.mono
                color: Theme.muted
            }
            Loader {
                id: actionLoader
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: isRemote ? downloadAction : scaleBadge
            }

            Component {
                id: scaleBadge
                // Hidden when the row label already states the scale
                // ("AnimeSharp-x4" + "x4"); only bare variant rows show it.
                Text {
                    visible: !/-x\d+/i.test(row.shortName || row.entry.name)
                    text: "x" + row.entry.scale
                    font.pixelSize: 12
                    font.family: Theme.mono
                    color: Theme.muted
                }
            }

            // Remote row: the 下载 button used to vanish while downloading,
            // leaving a bare percentage and no sense of how far along it was.
            // Keep a fixed-width slot: button, or a determinate bar + percent.
            Component {
                id: downloadAction
                Item {
                    width: 132
                    height: 28
                    Pill {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !row.downloading
                        compact: true
                        text: "下载"
                        enabled: snap.downloading === ""
                        onClicked: Desk.download(row.entry.name)
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: row.downloading
                        spacing: 8
                        Progress {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 84
                            from: 0
                            to: 100
                            value: Math.max(0, snap.downloadPct)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 34
                            horizontalAlignment: Text.AlignRight
                            text: Math.max(0, snap.downloadPct) + "%"
                            font.pixelSize: 12
                            font.family: Theme.mono
                            color: Theme.onSurface
                        }
                    }
                }
            }
        }

        TapHandler {
            enabled: !row.isRemote
            onTapped: Desk.setModel(row.entry.name)
        }
    }
}
