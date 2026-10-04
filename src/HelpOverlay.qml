import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, grouped. The README lists the same ones.
Rectangle {
    id: help
    color: "#e00e0e10"

    MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

    readonly property var groups: [
        ["Project", [["O", "Animate an image"], ["Shift+O", "Open an animation folder"], ["I", "Add an image as a layer"],
                     ["Ctrl+E", "Export MP4, WebM, GIF or PNG frames"], ["Ctrl+Z", "Undo"], ["Ctrl+Shift+Z", "Redo"],
                     ["?", "This list"], ["Q", "Quit"]]],
        ["Time", [["Space", "Play / pause"], [", .", "Previous / next frame"], ["< >", "Previous / next keyframe"],
                  ["Home End", "Start / end"]]],
        ["Layers", [["Click", "Select the layer under the mouse"], ["Drag", "Move it (Shift+drag keeps the selection)"],
                    ["Tab", "Select the layer below / Shift+Tab above"], ["Ctrl+↑ ↓", "Move it up / down the stack"],
                    ["H", "Hide / show"], ["F2", "Rename"], ["Delete", "Delete"]]],
        ["Animate", [["← → ↑ ↓", "Nudge 1px (Shift: 10px)"], ["[ ]", "Rotate 1° ({ }: 15°)"],
                     ["- =", "Scale down / up (or the mouse wheel)"], ["9 0", "Opacity down / up"],
                     ["K", "Keyframe everything at the playhead"], ["Drag ◆", "Retime keyframes (Shift+click picks more)"],
                     ["X", "Remove the picked keyframes, or those at the playhead"],
                     ["E", "Change their easing"], ["Ctrl+C Ctrl+V", "Copy them, paste at the playhead on any layer"]]],
        ["Break apart", [["C", "Cut tool: click around a piece"], ["Enter", "Cut it out, leaving a hole"],
                         ["Shift+Enter", "Copy it out, leaving the source whole"], ["Backspace", "Undo the last point"],
                         ["P", "Pivot tool: click where the layer turns"], ["Esc", "Cancel / deselect"]]]
    ]

    Flickable {
        anchors.fill: parent
        contentHeight: grid.implicitHeight + 80
        clip: true

        GridLayout {
            id: grid
            anchors.horizontalCenter: parent.horizontalCenter
            y: 40
            columns: help.width > 1200 ? 3 : 2
            columnSpacing: 56
            rowSpacing: 28

            Repeater {
                model: help.groups
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.alignment: Qt.AlignTop
                    spacing: 7
                    Label {
                        text: modelData[0]
                        color: theme.accent
                        font.bold: true
                        font.pixelSize: 16
                        Layout.bottomMargin: 4
                    }
                    Repeater {
                        model: modelData[1]
                        delegate: RowLayout {
                            required property var modelData
                            spacing: 14
                            Label {
                                text: modelData[0]
                                color: win.text
                                font.family: "monospace"
                                font.bold: true
                                Layout.preferredWidth: 110
                            }
                            Label { text: modelData[1]; color: "#b8b8c0" }
                        }
                    }
                }
            }
        }
    }

    Label {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 20
        text: "Everything saves itself to scene.json, and edits to that file show up here live.   ·   Esc closes"
        color: win.muted
    }
}
