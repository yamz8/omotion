import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, grouped. The README lists the same ones.
Rectangle {
    id: help
    color: "#f40e0e10"

    MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

    readonly property var groups: [
        ["Project", [["O", "Animate an image"], ["Shift+O", "Open an animation folder"], ["I", "Add an image as a layer"], ["Shift+I", "Add particles: snow, rain, dust, fireflies, leaves, embers"], ["Ctrl+M", "Add music or sound"],
                     ["Ctrl+E", "Export MP4, WebM, GIF or PNG frames"], ["Ctrl+Shift+K", "Move unused images to the trash"], ["Ctrl+Z", "Undo"], ["Ctrl+Shift+Z", "Redo"],
                     ["?", "This list"], ["Q", "Quit"]]],
        ["View", [["Ctrl+wheel", "Look closer, about the mouse"], ["Middle-drag", "Move the view"],
                  ["Ctrl+= Ctrl+-", "Zoom the view in / out"], ["Ctrl+0", "Fit the canvas back in the window"]]],
        ["Time", [["Space", "Play / pause"], ["L", "Show the layer's path and ghosts of it at the keys either side"], [", .", "Previous / next frame"], ["< >", "Previous / next keyframe"],
                  ["Home End", "Start / end"]]],
        ["Layers", [["Click", "Select the layer under the mouse"], ["Drag", "Move it (Shift+drag keeps the selection)"],
                    ["Tab", "Select the layer below / Shift+Tab above"], ["Ctrl+↑ ↓", "Move it up / down the stack"],
                    ["H", "Hide / show"], ["F2", "Rename"], ["Ctrl+D", "Duplicate"], ["Delete", "Delete"]]],
        ["Look", [["B", "Blend: normal, add, screen, multiply, overlay, lighten, darken"],
                  ["Inspector", "Brightness, contrast, saturation, blur, glow; ◇ keyframes them"]]],
        ["Camera", [["V", "Select the camera (or its row on the timeline)"], ["Drag", "Pan the view; the wheel zooms, [ ] turns"],
                    ["K", "Keyframe the camera"], ["Follow camera %", "In a layer's look: how much the camera moves it"]]],
        ["Animate", [["← → ↑ ↓", "Nudge 1px (Shift: 10px)"], ["[ ]", "Rotate 1° ({ }: 15°)"],
                     ["- =", "Scale down / up (or the mouse wheel; Shift+wheel turns)"], ["9 0", "Opacity down / up"],
                     ["N", "A ready-made motion: sway, float, pulse, bounce, spin, fade in, or one you saved"],
                     ["Ctrl+Shift+S", "Save the animation as a motion, for any project"],
                     ["K", "Keyframe everything at the playhead"], ["Drag ◆", "Retime keyframes (Shift+click picks more)"],
                     ["X", "Remove the picked keyframes, or those at the playhead"],
                     ["E", "Change their easing"], ["Ctrl+C Ctrl+V", "Copy them, paste at the playhead on any layer"]]],
        ["AI", [["D", "Split the picture into layers by depth"], ["A", "Make a depth split move: a parallax camera"],
                ["W", "AI select: click a thing, Shift+click adds, right-click leaves out"], ["T", "In AI select: its other guesses"],
                ["M", "In W: switch AI / colour select"]]],
        ["Break apart", [["W, M", "Colour select: click a part (Shift+click adds)"], ["Wheel / T", "Its tolerance (it picks one per click)"],
                         ["C", "Cut tool: drag around a piece, or click points"], ["Drag a point", "Move it (click an edge to add one)"],
                         ["Right-click", "Remove a point"], ["F", "Soft edge: 0, 1, 3, 8, 20 px"], ["G", "Fill the hole it leaves: AI, smooth, or none"],
                         ["Enter", "Cut it out, leaving a hole"], ["Shift+Enter", "Copy it out, leaving the source whole"],
                         ["R", "Reshape the selected piece (a depth layer: paint it)"], ["Shift+R", "Paint a piece's shape: drag adds, right-drag takes away"], ["P", "Pivot tool: click where the layer turns"],
                         ["Esc", "Cancel / deselect"]]]
    ]

    Flickable {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        contentHeight: grid.implicitHeight + 60
        clip: true

        GridLayout {
            id: grid
            // As many columns as fit, each wrapping its descriptions to suit.
            readonly property int count: help.width > 1300 ? 3 : help.width > 820 ? 2 : 1
            readonly property real columnWidth: (help.width - 64 - (count - 1) * columnSpacing) / count
            x: 32
            y: 40
            width: help.width - 64
            columns: count
            columnSpacing: 48
            rowSpacing: 28

            Repeater {
                model: help.groups
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: grid.columnWidth
                    Layout.maximumWidth: grid.columnWidth
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
                                Layout.preferredWidth: 120
                                Layout.alignment: Qt.AlignTop
                            }
                            Label {
                                text: modelData[1]
                                color: "#b8b8c0"
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
    }

    Label {
        id: footer
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 20
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: "Everything saves itself to scene.json, and edits to that file show up here live.   ·   Esc closes"
        color: win.muted
    }
}
