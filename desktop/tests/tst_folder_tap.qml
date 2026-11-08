import QtQuick
import QtQuick.Controls.Basic
import TstFolderTap

// Harness for tst_folder_tap: mounts the real Workbench in its empty state —
// the whole drop-zone / folder-action tree is live, at the geometry the app
// lays out (Column margins 16/8/16).
ApplicationWindow {
    id: win
    width: 900
    height: 600
    visible: true
    color: Theme.bg

    Workbench {
        objectName: "workbench"
        anchors.fill: parent
    }

    // Load-smoke only: parked off-window so it never intercepts clicks.
    ModelsPage {
        objectName: "modelsPage"
        x: 900
        width: 900
        height: 600
    }
}
// TEMP diagnostic: instantiate ModelsPage to surface its QML runtime errors.
