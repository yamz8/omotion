import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, grouped. The README lists the same ones.
Rectangle {
    id: help
    color: "#f40e0e10"

    MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

    readonly property var groups: [
        ["Project", [["O", "Animate an image"], ["Shift+O", "Open an animation folder"], ["I", "Add an image as a layer"], ["Shift+I", "Add particles: snow, rain, dust, fireflies, leaves, embers, smoke, fire, water"], ["Ctrl+M", "Add music or sound"],
                     ["Ctrl+E", "Export MP4, WebM, GIF, PNG frames, or glTF for Blender"], ["Ctrl+Shift+K", "Move unused images to the trash"], ["Ctrl+Z", "Undo"], ["Ctrl+Shift+Z", "Redo"],
                     ["F3 / Ctrl+K", "Say or search: find any operator by name, or say what you want and Claude Code does it"],
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
                     ["J", "Pins: click to pin, drag a pin to bend the layer, right-click to unpin"],
                     ["Shift+P", "Path: click points for the layer to follow from the playhead (Shift+Enter turns with it)"],
                     ["Shift+A", "Add an empty, a point to aim at"],
                     ["Shift+B", "Draw strokes, frame by frame (right-click takes one back)"],
                     ["Shift+T", "Track points on a video: Enter follows them (more than one solves its move), Shift+Enter holds it still"],
                     ["Ctrl+Shift+F", "Film: this project's shots in a row, previewed (Space plays), joined and exported"],
                     ["Inspector ▸ Rig", "Reach (IK arm) for an empty, look at a layer, roll as a wheel on a body"],
                     ["N", "A ready-made motion: sway, float, pulse, bounce, spin, fade in, or one you saved"],
                     ["Ctrl+Shift+S", "Save the animation as a motion, for any project"],
                     ["K", "Keyframe everything at the playhead"], ["Drag ◆", "Retime keyframes (Shift+click picks more)"],
                     ["X", "Remove the picked keyframes, or those at the playhead"],
                     ["E", "Change their easing"],
                     ["Ctrl+Shift+Tab", "Dope sheet (or ▸ on a track): a row a property; drag its keys alone, right-click takes one away"],
                     ["Ctrl+Tab", "Graph editor: the properties as curves; drag a key in time and value (Shift: value only), click a property to see it alone"], ["Ctrl+C Ctrl+V", "Copy them, paste at the playhead on any layer"]]],
        ["World", [["Shift+D", "Make the picture a 3D world (the AI reads its distances, lens and ground)"],
                   ["S", "Give the selected piece a 3D shape"],
                   ["V then drag", "Turn the camera; Shift+drag moves it"],
                   ["Drag a piece", "Move it in 3D; Shift+drag nearer / farther"],
                   ["Click a piece", "Pick it where it is in 3D; drag its arrows along x, y, z, its rings turn it (Ctrl snaps)"],
                   ["Ctrl+click", "Add to the selection: they move together"],
                   ["G", "Grab it: it follows the mouse; X, Y, Z keep to an axis, click places, Esc puts back"],
                   ["`", "Free view: look at the world from anywhere, the shot's camera shown in it"],
                   ["Shift+L", "Add a light: it adds to the picture's light on what faces it"],
                   ["Middle-drag / Alt+drag", "In the free view: orbit (Shift pans); the wheel or - = comes closer"],
                   ["← → ↑ ↓", "In the free view: orbit (Shift pans); F looks at the selected"],
                   ["Inspector", "Its real height, and a drive over the ground with jumps, dust and a shadow"],
                   ["Outliner", "The camera, the world's pieces under what they were cut from, the layers over it; World there sets haze, grain, shutter, dust"]]],
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
