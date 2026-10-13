import QtQuick
import QtQuick.Controls.Basic
import TstCompare

// Harness for tst_compare_drag: mounts the real CompareStage at the same
// rail offset the shell uses (rail width 76), so scene coordinates in the test
// match what a user's cursor sees.
ApplicationWindow {
    id: win
    width: 1280
    height: 840
    visible: true
    color: Theme.bg

    Item {
        x: 76
        y: 0
        width: parent.width - 76
        height: parent.height

        CompareStage {
            id: cmp
            objectName: "compareStage"
            anchors.fill: parent
            before: "file://" + FIXTURE_DIR + "/scene.svg"
            after: "file://" + FIXTURE_DIR + "/scene-after.svg"
        }
    }
}
