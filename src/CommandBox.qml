import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Say or search (F3): type to find any operator and Enter runs it, as
// Blender's search does; or say what you want in words, and the agent does
// it through scene.json and the command line, the window following along.
Rectangle {
    id: box
    color: "#b0000000"

    signal closed()

    // Every operator: its name, its key, whether it can run now, and what it
    // does. The keys stay where they are; this finds them by name.
    readonly property var operators: [
        ["Open an image to animate", "O", () => true, () => backend.openImageDialog()],
        ["Open an animation folder", "Shift+O", () => true, () => backend.openFolderDialog()],
        ["Add an image as a layer", "I", () => win.idle, () => backend.importImageDialog()],
        ["Add particles: snow, rain, dust, fireflies, leaves, embers", "Shift+I", () => win.idle, () => win.particleMenuVisible = true],
        ["Add music or sound", "Ctrl+M", () => win.idle, () => backend.importAudioDialog()],
        ["Export a video, GIF or frames", "Ctrl+E", () => win.idle && !backend.exporting, () => win.exportMenuVisible = true],
        ["Clean up unused images", "Ctrl+Shift+K", () => win.idle, () => backend.cleanUp()],
        ["Undo", "Ctrl+Z", () => backend.canUndo, () => backend.undo()],
        ["Redo", "Ctrl+Shift+Z", () => backend.canRedo, () => backend.redo()],
        ["Every shortcut", "?", () => true, () => win.helpVisible = true],
        ["Play / pause", "Space", () => win.idle, () => backend.togglePlay()],
        ["Next frame", ".", () => win.idle, () => backend.stepFrames(1)],
        ["Previous frame", ",", () => win.idle, () => backend.stepFrames(-1)],
        ["Next keyframe", ">", () => win.idle, () => backend.jumpKey(1)],
        ["Previous keyframe", "<", () => win.idle, () => backend.jumpKey(-1)],
        ["Go to the start", "Home", () => win.idle, () => backend.jumpTo(false)],
        ["Go to the end", "End", () => win.idle, () => backend.jumpTo(true)],
        ["Select the camera", "V", () => win.idle, () => backend.selectCamera()],
        ["Select the next layer", "Tab", () => win.idle, () => backend.selectNext(-1)],
        ["Hide / show the layer", "H", () => (win.idle && backend.selected >= 0), () => backend.toggleVisible(backend.selected)],
        ["Duplicate the layer", "Ctrl+D", () => (win.idle && backend.selected >= 0), () => backend.duplicateLayer()],
        ["Delete the layer", "Delete", () => (win.idle && backend.selected >= 0), () => backend.deleteLayer()],
        ["Rename the layer", "F2", () => (win.idle && backend.selected >= 0), () => timeline.rename(backend.selected)],
        ["Move the layer up the stack", "Ctrl+↑", () => (win.idle && backend.selected >= 0), () => backend.moveLayer(1)],
        ["Move the layer down the stack", "Ctrl+↓", () => (win.idle && backend.selected >= 0), () => backend.moveLayer(-1)],
        ["Blend the layer: add, screen, multiply…", "B", () => (win.idle && backend.selected >= 0), () => backend.cycleBlend()],
        ["Keyframe everything at the playhead", "K", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => backend.keyAll()],
        ["Remove keyframes", "X", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => backend.removeKeys()],
        ["Change the easing", "E", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => backend.cycleEase()],
        ["Copy keyframes", "Ctrl+C", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => backend.copyKeys()],
        ["Paste keyframes", "Ctrl+V", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => backend.pasteKeys()],
        ["A ready-made motion: sway, float, pulse, bounce, spin, fade", "N", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => win.presetMenuVisible = true],
        ["Save the animation as a motion", "Ctrl+Shift+S", () => (win.idle && (backend.selected >= 0 || backend.cameraSelected)), () => win.saveMotionVisible = true],
        ["Show the layer's path and ghosts", "L", () => win.idle, () => backend.showMotion = !backend.showMotion],
        ["Graph editor: curves", "Ctrl+Tab", () => win.idle, () => timeline.curves = !timeline.curves],
        ["Pins: bend the layer", "J", () => (win.idle && backend.selected >= 0), () => backend.setTool("pin")],
        ["Cut tool", "C", () => win.idle, () => backend.setTool("cut")],
        ["AI select: click a thing to cut it out", "W", () => win.idle, () => backend.setTool("wand")],
        ["Pivot tool: where the layer turns", "P", () => win.idle, () => backend.setTool("pivot")],
        ["Reshape the piece", "R", () => (win.idle && backend.selected >= 0), () => backend.startReshape()],
        ["Paint the piece's shape", "Shift+R", () => (win.idle && backend.selected >= 0), () => backend.startBrush()],
        ["Split the picture into layers by depth (AI)", "D", () => win.idle, () => backend.splitByDepth(4)],
        ["Parallax camera move for a depth split", "A", () => win.idle, () => backend.animateParallax()],
        ["Make the picture a 3D world (AI)", "Shift+D", () => win.idle, () => backend.makeWorld()],
        ["Give the piece a 3D shape (AI)", "S", () => (win.idle && backend.selected >= 0), () => backend.shapeSelected()],
        ["Free view: look around the world", "`", () => win.idle && backend.world.active, () => backend.toggleFreeView()],
        ["Look at the selected piece", "F", () => backend.freeView, () => backend.frameSelected()],
        ["Fill the hole behind it by the AI", "", () => (win.idle && backend.selected >= 0) && box.canFill(), () => backend.runStep("fill", "ai")],
        ["Fill the hole behind it smoothly", "", () => (win.idle && backend.selected >= 0) && box.canFill(), () => backend.runStep("fill", "smooth")],
        ["Leave the hole behind it open", "", () => (win.idle && backend.selected >= 0) && box.canFill(), () => backend.runStep("fill", "none")],
        ["Redo what's out of date (AI)", "", () => win.idle && backend.staleSteps > 0, () => backend.redoStale()]
    ]

    property var matches: []
    property int current: 0
    readonly property bool asking: field.text.trim() !== ""
    // The rows: the operators found, then asking the agent.
    readonly property int count: matches.length + (asking ? 1 : 0)

    function canFill() {
        const steps = backend.steps;
        for (let i = 0; i < steps.length; ++i)
            if (steps[i].name === "fill" && steps[i].actions.length > 0)
                return true;
        return false;
    }
    function refresh() {
        const names = [], usable = [];
        for (let i = 0; i < operators.length; ++i)
            if (operators[i][2]()) {
                usable.push(i);
                names.push(operators[i][0]);
            }
        const order = backend.rankCommands(field.text, names);
        const found = [];
        for (let i = 0; i < order.length && found.length < 9; ++i)
            found.push(usable[order[i]]);
        matches = found;
        // Words that match nothing are for the agent.
        current = found.length === 0 && asking ? found.length : 0;
    }
    function open() {
        field.text = "";
        refresh();
        visible = true;
        field.forceActiveFocus();
    }
    function close() {
        visible = false;
        closed();
    }
    function run(row) {
        const text = field.text.trim();
        close();
        if (row < matches.length)
            operators[matches[row]][3]();
        else if (text !== "")
            backend.askAgent(text);
    }

    visible: false
    MouseArea { anchors.fill: parent; onClicked: box.close() }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * .12
        width: Math.min(620, parent.width - 40)
        height: column.implicitHeight + 24
        radius: 10
        color: win.panel
        border.color: win.line
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 12
            spacing: 4

            TextField {
                id: field
                Layout.fillWidth: true
                placeholderText: "Search, or say what you want: “make the car jump over the crest”"
                font.pixelSize: 16
                onTextChanged: box.refresh()
                Keys.onEscapePressed: box.close()
                Keys.onUpPressed: box.current = Math.max(0, box.current - 1)
                Keys.onDownPressed: box.current = Math.min(box.count - 1, box.current + 1)
                Keys.onReturnPressed: function (event) {
                    // Ctrl or Shift+Enter always asks.
                    box.run((event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) ? box.matches.length : box.current);
                }
                Keys.onEnterPressed: function (event) { box.run((event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) ? box.matches.length : box.current) }
            }

            Repeater {
                model: box.count
                delegate: Rectangle {
                    required property int index
                    readonly property bool isAsk: index >= box.matches.length
                    Layout.fillWidth: true
                    height: 34
                    radius: 6
                    color: box.current === index ? "#1e2130" : rowMouse.containsMouse ? "#18181c" : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: parent.parent.isAsk ? theme.accent : win.text
                            text: parent.parent.isAsk
                                  ? (backend.agentAvailable ? "Ask Claude: “" + field.text.trim() + "”" : "Asking in words needs Claude Code installed")
                                  : box.operators[box.matches[index]][0]
                        }
                        Label {
                            color: win.muted
                            text: parent.parent.isAsk ? (box.matches.length ? "Ctrl+Enter" : "Enter")
                                                       : box.operators[box.matches[index]][1]
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: box.run(index)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 4
                wrapMode: Text.Wrap
                color: win.muted
                font.pixelSize: 12
                text: "↑ ↓ pick   ·   Enter runs   ·   Ctrl+Enter asks Claude   ·   Esc closes"
                      + (backend.agentAvailable ? "\nClaude Code works on this project through scene.json and omotion's commands (it talks to Anthropic); "
                                                  + "you'll see what it changed, and one undo takes it back." : "")
            }
        }
    }
}
