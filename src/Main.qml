import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omotion

ApplicationWindow {
    id: win
    width: 1360
    height: 860
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: backend.hasProject ? backend.projectName + " — omotion" : "omotion"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent

    readonly property color panel: "#141417"
    readonly property color line: "#26262b"
    readonly property color muted: "#8a8a93"
    readonly property color text: "#e6e6ea"

    property bool helpVisible: false
    property bool exportMenuVisible: false
    property bool exportHalf: false
    property bool presetMenuVisible: false
    property bool particleMenuVisible: false
    property bool saveMotionVisible: false
    property bool quitConfirmVisible: false
    property bool filmVisible: false
    readonly property bool commandBoxVisible: commandBox.visible
    // Editing shortcuts go quiet while an overlay is up.
    // Ready but for the search box: what its operators can do once it closes.
    readonly property bool idle: backend.hasProject && !helpVisible && !exportMenuVisible && !quitConfirmVisible && !presetMenuVisible && !particleMenuVisible && !saveMotionVisible && !filmVisible
                                 && backend.aiPrompt === "" && backend.busy === ""
    readonly property bool ready: idle && !commandBoxVisible
    readonly property bool hasLayer: ready && backend.selected >= 0
    // Something whose properties edits act on: a layer, or the camera.
    readonly property bool hasTarget: ready && (backend.selected >= 0 || backend.cameraSelected)

    function quit() {
        if (backend.exporting && !quitConfirmVisible) {
            quitConfirmVisible = true;
            return;
        }
        backend.cancelExport();
        backend.flush();
        Qt.quit();
    }
    function focusCanvas() { canvasArea.forceActiveFocus(); }
    // Exports have even sides (H.264 needs them); show the size they'll be.
    function evenSize(v) { const n = Math.max(2, Math.round(v)); return n - n % 2; }

    onClosing: function (close) {
        if (backend.exporting && !quitConfirmVisible) {
            close.accepted = false;
            quitConfirmVisible = true;
        }
    }

    // ---- Shortcuts -------------------------------------------------------

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: { win.exportMenuVisible = false; win.presetMenuVisible = false; win.particleMenuVisible = false; win.helpVisible = !win.helpVisible } }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.helpVisible; onActivated: win.quit() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.quitConfirmVisible) win.quitConfirmVisible = false;
            else if (commandBox.visible) commandBox.close();
            else if (win.filmVisible) win.filmVisible = false;
            else if (backend.agentBusy) backend.cancelAgent();
            else if (backend.agentReview) backend.takeBackAgent();
            else if (backend.handle >= 0) backend.endHandle(false);
            else if (backend.aiPrompt !== "") backend.dismissAiPrompt();
            else if (backend.downloading) backend.cancelDownload();
            else if (win.helpVisible) win.helpVisible = false;
            else if (win.exportMenuVisible) win.exportMenuVisible = false;
            else if (win.presetMenuVisible) win.presetMenuVisible = false;
            else if (win.particleMenuVisible) win.particleMenuVisible = false;
            else if (win.saveMotionVisible) win.saveMotionVisible = false;
            else if (backend.exporting) backend.cancelExport();
            else if (backend.tool !== "move") backend.setTool("move");
            else if (backend.keySelection.length > 0) backend.clearKeySelection();
            else if (backend.error !== "") backend.dismissError();
            else if (!canvasArea.activeFocus) win.focusCanvas();
            else backend.select(-1);
            win.focusCanvas();
        }
    }

    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: !win.helpVisible; onActivated: backend.openImageDialog() }
    Shortcut { sequence: "Shift+O"; context: Qt.ApplicationShortcut; enabled: !win.helpVisible; onActivated: backend.openFolderDialog() }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.importImageDialog() }
    Shortcut { sequence: "Shift+I"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: win.particleMenuVisible = true }
    Shortcut { sequence: "Ctrl+M"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.importAudioDialog() }
    Shortcut { sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: win.saveMotionVisible = true }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; enabled: win.ready && !backend.exporting; onActivated: win.exportMenuVisible = true }

    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.togglePlay() }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.redo() }

    Shortcut { sequence: ","; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.stepFrames(-1) }
    Shortcut { sequence: "."; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.stepFrames(1) }
    Shortcut { sequence: "<"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.jumpKey(-1) }
    Shortcut { sequence: ">"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.jumpKey(1) }
    Shortcut { sequence: "Home"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.jumpTo(false) }
    Shortcut { sequence: "End"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.jumpTo(true) }

    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.selectNext(-1) }
    Shortcut { sequences: ["Backtab", "Shift+Tab"]; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.selectNext(1) }
    Shortcut { sequence: "Ctrl+Up"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.moveLayer(1) }
    Shortcut { sequence: "Ctrl+Down"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.moveLayer(-1) }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.toggleVisible(backend.selected) }
    Shortcut { sequence: "Delete"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.deleteLayer() }
    Shortcut { sequence: "F2"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: timeline.rename(backend.selected) }
    Shortcut { sequence: "Ctrl+D"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.duplicateLayer() }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.selectCamera() }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: view.zoomBy(1.25, view.width / 2, view.height / 2) }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: view.zoomBy(1 / 1.25, view.width / 2, view.height / 2) }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: view.fit() }
    Shortcut { sequence: "J"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setTool(backend.tool === "pin" ? "move" : "pin") }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.showMotion = !backend.showMotion }
    Shortcut { sequence: "Ctrl+Shift+K"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.cleanUp() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.cycleBlend() }
    Shortcut { sequence: "Shift+D"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.busy === ""; onActivated: backend.makeWorld() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.hasLayer && backend.busy === ""; onActivated: backend.shapeSelected() }

    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("x", -1) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("x", 1) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("y", -1) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("y", 1) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("x", -10) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("x", 10) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("y", -10) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.adjust("y", 10) }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.adjust("rotation", -1) }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.adjust("rotation", 1) }
    Shortcut { sequence: "{"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.adjust("rotation", -15) }
    Shortcut { sequence: "}"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.adjust("rotation", 15) }
    Shortcut { sequence: "-"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.multiply("scale", 1 / 1.05) }
    Shortcut { sequences: ["=", "+"]; context: Qt.ApplicationShortcut; enabled: win.hasTarget && !backend.freeView; onActivated: backend.multiply("scale", 1.05) }
    Shortcut { sequence: "9"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setProperty("opacity", Math.max(0, backend.values.opacity - 0.1)) }
    Shortcut { sequence: "0"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setProperty("opacity", Math.min(1, backend.values.opacity + 0.1)) }

    Shortcut { sequence: "K"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.keyAll() }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && backend.handle < 0; onActivated: backend.removeKeys() }
    Shortcut { sequence: "E"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.cycleEase() }
    Shortcut { sequence: "Ctrl+C"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.copyKeys() }
    Shortcut { sequence: "Ctrl+V"; context: Qt.ApplicationShortcut; enabled: win.hasTarget; onActivated: backend.pasteKeys() }

    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.setTool(backend.tool === "cut" ? "move" : "cut") }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.setTool(backend.tool === "pivot" ? "move" : "pivot") }
    // A path for the selected layer to follow, drawn as a cut's points are.
    Shortcut { sequence: "Shift+P"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setTool(backend.tool === "path" ? "move" : "path") }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "path"; onActivated: backend.finishPath(false) }
    Shortcut { sequences: ["Shift+Return", "Shift+Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "path"; onActivated: backend.finishPath(true) }
    Shortcut { sequence: "Shift+A"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.addEmpty() }
    // Drawing, as Grease Pencil: strokes on a drawing layer, frame by frame.
    Shortcut { sequence: "Shift+B"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.startDrawing() }
    Shortcut { sequence: "Shift+L"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.addLight() }
    Shortcut { sequence: "Shift+T"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setTool(backend.tool === "track" ? "move" : "track") }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: win.hasLayer && backend.tool === "move"; onActivated: backend.startReshape() }
    Shortcut { sequence: "Shift+R"; context: Qt.ApplicationShortcut; enabled: win.hasLayer && backend.tool === "move"; onActivated: backend.startBrush() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "brush"; onActivated: backend.finishBrush() }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.ready && (backend.tool === "cut" || backend.tool === "wand"); onActivated: backend.cycleFeather() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: win.ready && (backend.tool === "cut" || backend.tool === "wand"); onActivated: backend.toggleFill() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && (backend.tool === "cut" || backend.tool === "wand"); onActivated: backend.finishCut(true) }
    Shortcut { sequences: ["Shift+Return", "Shift+Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && (backend.tool === "cut" || backend.tool === "wand"); onActivated: backend.finishCut(false) }
    Shortcut { sequence: "W"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.setTool(backend.tool === "wand" ? "move" : "wand") }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "wand"; onActivated: backend.cycleTolerance() }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "wand"; onActivated: backend.toggleWandMode() }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "move"; onActivated: backend.splitByDepth(4) }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "move"; onActivated: backend.animateParallax() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.hasTarget && backend.tool === "move"; onActivated: win.presetMenuVisible = true }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: backend.aiPrompt !== ""; onActivated: backend.acceptAiPrompt() }

    // The 3D view: the free view, looking at what's selected, and grabbing
    // a piece of the world (G), kept to an axis with X, Y or Z.
    // Say or search: every operator by name, or words for the agent.
    Shortcut { sequences: ["F3", "Ctrl+K"]; context: Qt.ApplicationShortcut; enabled: backend.hasProject && !backend.agentBusy && !commandBox.visible; onActivated: commandBox.open() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: backend.agentReview && backend.tool === "move" && !commandBox.visible; onActivated: backend.keepAgent() }
    Shortcut { sequence: "Ctrl+Shift+F"; context: Qt.ApplicationShortcut; enabled: backend.hasProject && !commandBox.visible; onActivated: win.filmVisible = !win.filmVisible }
    // Space plays the film while its panel is open, as it plays the scene otherwise.
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.filmVisible && backend.filmShots.length > 0; onActivated: backend.toggleFilmPlay() }
    Shortcut { sequence: "Ctrl+Tab"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: timeline.curves = !timeline.curves }
    // The dope sheet, on the tracks rather than the curves.
    Shortcut { sequence: "Ctrl+Shift+Tab"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: { timeline.curves = false; timeline.sheet = !timeline.sheet } }
    Shortcut { sequence: "`"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.handle < 0; onActivated: backend.toggleFreeView() }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView && backend.tool === "move"; onActivated: backend.frameSelected() }
    Shortcut {
        sequence: "G"
        context: Qt.ApplicationShortcut
        enabled: win.ready && backend.tool === "move" && backend.handle < 0
        onActivated: {
            const p = view.toView(canvasMouse.mouseX, canvasMouse.mouseY);
            backend.beginHandle(0, p.x, p.y);
            canvasMouse.grabbing = backend.handle >= 0;
        }
    }
    // In the free view the arrows orbit (Shift pans) and - and = come
    // closer, as Blender's number pad does.
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.orbitView(-50, 0) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.orbitView(50, 0) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.orbitView(0, -50) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.orbitView(0, 50) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.panView(-60, 0) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.panView(60, 0) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.panView(0, -60) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.panView(0, 60) }
    Shortcut { sequence: "-"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.zoomView(1 / 1.25) }
    Shortcut { sequences: ["=", "+"]; context: Qt.ApplicationShortcut; enabled: win.ready && backend.freeView; onActivated: backend.zoomView(1.25) }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: backend.handle >= 0; onActivated: backend.setHandleAxis(1) }
    Shortcut { sequence: "Y"; context: Qt.ApplicationShortcut; enabled: backend.handle >= 0; onActivated: backend.setHandleAxis(2) }
    Shortcut { sequence: "Z"; context: Qt.ApplicationShortcut; enabled: backend.handle >= 0; onActivated: backend.setHandleAxis(3) }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: backend.handle >= 0; onActivated: { backend.endHandle(true); canvasMouse.grabbing = false } }
    Shortcut { sequence: "Backspace"; context: Qt.ApplicationShortcut; enabled: win.ready && (backend.tool === "cut" || backend.tool === "path"); onActivated: backend.removeCutPoint() }

    // ---- Layout ----------------------------------------------------------

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Top bar: what's open, what just happened, and the export.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: win.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 10
                spacing: 12

                Label {
                    text: "omotion"
                    font.bold: true
                    color: theme.accent
                }
                Label {
                    text: backend.projectName
                    color: win.text
                    elide: Text.ElideRight
                    Layout.maximumWidth: 260
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignRight
                    text: backend.notice
                    color: win.muted
                    elide: Text.ElideRight
                }
                ProgressBar {
                    visible: backend.downloading
                    value: backend.downloadProgress
                    Layout.preferredWidth: 140
                }
                Label {
                    visible: backend.downloading
                    text: "Downloading the AI models " + Math.round(backend.downloadProgress * 100) + "%  ·  Esc cancels"
                    color: win.muted
                }
                ProgressBar {
                    visible: backend.exporting
                    value: backend.exportProgress
                    Layout.preferredWidth: 140
                }
                Label {
                    visible: backend.exporting
                    text: "Exporting " + Math.round(backend.exportProgress * 100) + "%  ·  Esc cancels"
                    color: win.muted
                }
                ToolButton {
                    text: "Say or search…  F3"
                    visible: backend.hasProject && !backend.agentBusy
                    onClicked: commandBox.open()
                }
                ToolButton {
                    // AI steps made from something that has changed since.
                    visible: backend.staleSteps > 0 && backend.busy === ""
                    text: "⚠ " + backend.staleSteps + " out of date · redo"
                    Material.foreground: "#ffb35c"
                    onClicked: { backend.redoStale(); win.focusCanvas() }
                    ToolTip.visible: hovered
                    ToolTip.text: "The world or a shape was made from pieces that have changed since. Redo them, in order, with the AI"
                }
                ToolButton {
                    text: backend.freeView ? "Free view" : "Camera view"
                    visible: backend.hasProject && backend.world.active
                    onClicked: { backend.toggleFreeView(); win.focusCanvas() }
                    ToolTip.visible: hovered
                    ToolTip.text: "` switches between the shot's camera and a free view of the world"
                }
                ToolButton {
                    text: "Export"
                    visible: backend.hasProject && !backend.exporting
                    onClicked: win.exportMenuVisible = true
                }
                ToolButton {
                    text: "?"
                    onClicked: win.helpVisible = !win.helpVisible
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.line }
        }

        // Errors stay until dismissed, so nothing goes wrong quietly.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: errorText.implicitHeight + 16
            visible: backend.error !== ""
            color: "#3a1518"

            Label {
                id: errorText
                anchors.left: parent.left
                anchors.right: dismiss.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 14
                text: backend.error
                color: "#ffb4b4"
                wrapMode: Text.Wrap
            }
            ToolButton {
                id: dismiss
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "✕"
                onClicked: backend.dismissError()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The canvas.
            Item {
                id: canvasArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                focus: true
                clip: true

                // A world, in 3D, exactly where the canvas is; the canvas
                // draws everything else over it.
                Rectangle {
                    // With a look, the canvas draws the world itself, as exported.
                    visible: backend.hasProject && backend.world.active && (backend.freeView || !backend.sceneInfo.looked)
                    // The free view fills the whole area.
                    x: backend.freeView ? 0 : view.canvasRect.x
                    y: backend.freeView ? 0 : view.canvasRect.y
                    width: backend.freeView ? canvasArea.width : view.canvasRect.width
                    height: backend.freeView ? canvasArea.height : view.canvasRect.height
                    // The free view stands on grey, as a 3D editor does.
                    color: backend.freeView ? "#26262b" : backend.sceneInfo.background || "black"
                    clip: true
                    WorldView {
                        anchors.fill: parent
                        source: backend.world
                    }
                }

                SceneView {
                    id: view
                    anchors.fill: parent
                    editor: backend
                    accent: theme.accent
                    visible: backend.hasProject
                }

                MouseArea {
                    id: canvasMouse
                    anchors.fill: parent
                    enabled: backend.hasProject && backend.busy === ""
                    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                    property point panFrom
                    cursorShape: backend.tool === "brush" ? Qt.BlankCursor : backend.tool === "move" ? (pressed && backend.selected >= 0 ? Qt.ClosedHandCursor : Qt.ArrowCursor) : Qt.CrossCursor
                    property bool dragging: false
                    // In the cut tool: the point being dragged, or a lasso
                    // being drawn, and where it last dropped a point.
                    property int cutPoint: -1
                    property int pinDrag: -1
                    // The mouse is followed everywhere: G grabs from where it is.
                    hoverEnabled: true
                    // A grab (G) follows the mouse until a click.
                    property bool grabbing: false
                    property bool handling: false
                    property bool orbiting: false
                    property bool lassoing: false
                    property point lassoAt

                    onPressed: function (mouse) {
                        win.focusCanvas();
                        if (grabbing) {
                            backend.endHandle(mouse.button === Qt.LeftButton);
                            grabbing = false;
                            return;
                        }
                        // The middle button pans the view, whatever the tool;
                        // in the free view it orbits (Shift pans), and so
                        // does Alt with the left button, for touchpads.
                        if (mouse.button === Qt.MiddleButton || (backend.freeView && (mouse.modifiers & Qt.AltModifier))) {
                            panFrom = Qt.point(mouse.x, mouse.y);
                            orbiting = backend.freeView;
                            return;
                        }
                        const p = view.toScene(mouse.x, mouse.y);
                        if (backend.tool === "cut" || backend.tool === "path") {
                            const reach = 9 / view.zoom();
                            const near = backend.cutPointNear(p.x, p.y, reach);
                            if (mouse.button === Qt.RightButton) {
                                if (near >= 0) backend.removeCutPointAt(near);
                                else backend.removeCutPoint();
                                return;
                            }
                            cutPoint = near >= 0 ? near : backend.insertCutPoint(p.x, p.y, reach * 0.7);
                            if (cutPoint < 0) {
                                backend.addCutPoint(p.x, p.y);
                                lassoing = true;
                                lassoAt = Qt.point(mouse.x, mouse.y);
                            }
                            return;
                        }
                        if (backend.tool === "brush") {
                            backend.beginBrush(p.x, p.y, mouse.button === Qt.RightButton);
                            return;
                        }
                        if (backend.tool === "draw") {
                            if (mouse.button === Qt.RightButton) backend.undoStroke();
                            else backend.beginStroke(p.x, p.y);
                            return;
                        }
                        if (backend.tool === "pin") {
                            const near = backend.pinNear(p.x, p.y, 10 / view.zoom());
                            if (mouse.button === Qt.RightButton) {
                                if (near >= 0) backend.removePin(near);
                                return;
                            }
                            pinDrag = near >= 0 ? near : backend.addPinAt(p.x, p.y);
                            if (pinDrag >= 0) backend.beginPinDrag(pinDrag, p.x, p.y);
                            return;
                        }
                        if (backend.tool === "wand") {
                            if (mouse.button === Qt.RightButton)
                                backend.excludeAt(p.x, p.y);
                            else
                                backend.smartSelect(p.x, p.y, (mouse.modifiers & Qt.ShiftModifier) !== 0);
                            return;
                        }
                        if (backend.tool === "pivot") {
                            backend.setPivotAt(p.x, p.y);
                            return;
                        }
                        if (backend.tool === "track") {
                            backend.trackAt(p.x, p.y);
                            return;
                        }
                        // A handle on what's selected in the world moves it.
                        const v = view.toView(mouse.x, mouse.y);
                        if (mouse.button === Qt.LeftButton) {
                            const h = backend.handleAt(v.x, v.y, 9 / view.viewZoom());
                            if (h >= 0) {
                                backend.beginHandle(h, v.x, v.y);
                                handling = true;
                                return;
                            }
                        }
                        // With the camera selected, dragging pans the view.
                        // Otherwise, Shift drags the selected layer even
                        // when another one is on top of it.
                        // Ctrl+click adds to the selection (or takes out).
                        if (mouse.modifiers & Qt.ControlModifier) {
                            backend.pickAt(v.x, v.y, true);
                            return;
                        }
                        if (!backend.cameraSelected && !(mouse.modifiers & Qt.ShiftModifier))
                            backend.pickAt(v.x, v.y);
                        // In the free view, a piece of the world moves across
                        // the view as it's dragged.
                        if (backend.freeView) {
                            if (mouse.button === Qt.LeftButton) {
                                backend.beginHandle(0, v.x, v.y);
                                handling = backend.handle >= 0;
                            }
                            return;
                        }
                        if ((backend.selected >= 0 || backend.cameraSelected) && mouse.button === Qt.LeftButton) {
                            backend.beginDrag(p.x, p.y, (mouse.modifiers & Qt.ShiftModifier) !== 0);
                            dragging = true;
                        }
                    }
                    onPositionChanged: function (mouse) {
                        if (grabbing || handling) {
                            const g = view.toView(mouse.x, mouse.y);
                            // Ctrl snaps: 10 cm steps, 15° turns.
                            backend.handleTo(g.x, g.y, (mouse.modifiers & Qt.ControlModifier) !== 0);
                            return;
                        }
                        if (orbiting) {
                            if (mouse.modifiers & Qt.ShiftModifier)
                                backend.panView(mouse.x - panFrom.x, mouse.y - panFrom.y);
                            else
                                backend.orbitView(mouse.x - panFrom.x, mouse.y - panFrom.y);
                            panFrom = Qt.point(mouse.x, mouse.y);
                            return;
                        }
                        if (pressedButtons & Qt.MiddleButton) {
                            view.panBy(mouse.x - panFrom.x, mouse.y - panFrom.y);
                            panFrom = Qt.point(mouse.x, mouse.y);
                            return;
                        }
                        if (pinDrag >= 0) {
                            const q = view.toScene(mouse.x, mouse.y);
                            backend.pinDragTo(q.x, q.y);
                            return;
                        }
                        if (backend.tool === "draw") {
                            if (pressed) {
                                const d = view.toScene(mouse.x, mouse.y);
                                backend.strokeTo(d.x, d.y);
                            }
                            return;
                        }
                        if (backend.tool === "brush") {
                            if (pressed) {
                                const b = view.toScene(mouse.x, mouse.y);
                                backend.brushTo(b.x, b.y);
                            }
                            return;
                        }
                        if (cutPoint >= 0) {
                            const q = view.toScene(mouse.x, mouse.y);
                            backend.moveCutPoint(cutPoint, q.x, q.y);
                            return;
                        }
                        // Dragging traces the shape freehand, a point every few pixels.
                        if (lassoing && Math.hypot(mouse.x - lassoAt.x, mouse.y - lassoAt.y) >= 6) {
                            const q = view.toScene(mouse.x, mouse.y);
                            backend.addCutPoint(q.x, q.y);
                            lassoAt = Qt.point(mouse.x, mouse.y);
                            return;
                        }
                        if (!dragging)
                            return;
                        const p = view.toScene(mouse.x, mouse.y);
                        backend.dragTo(p.x, p.y);
                    }
                    onReleased: {
                        if (handling) {
                            backend.endHandle(true);
                            handling = false;
                        }
                        orbiting = false;
                        if (backend.tool === "brush")
                            backend.endBrush();
                        if (backend.tool === "draw")
                            backend.endStroke();
                        if (pinDrag >= 0) {
                            backend.endPinDrag();
                            pinDrag = -1;
                        }
                        if (dragging)
                            backend.endDrag();
                        dragging = false;
                        cutPoint = -1;
                        lassoing = false;
                    }
                    onWheel: function (wheel) {
                        if (backend.freeView) {
                            backend.zoomView(wheel.angleDelta.y > 0 ? 1.15 : 1 / 1.15);
                            return;
                        }
                        // Ctrl looks closer, about the mouse.
                        if (wheel.modifiers & Qt.ControlModifier) {
                            view.zoomBy(wheel.angleDelta.y > 0 ? 1.15 : 1 / 1.15, wheel.x, wheel.y);
                            return;
                        }
                        if (backend.tool === "brush" || backend.tool === "draw") {
                            backend.adjustBrush(wheel.angleDelta.y > 0 ? 1.15 : 1 / 1.15);
                            return;
                        }
                        if (backend.tool === "wand") {
                            backend.adjustTolerance(wheel.angleDelta.y > 0 ? 4 : -4);
                            return;
                        }
                        if (backend.selected < 0 && !backend.cameraSelected)
                            return;
                        const step = wheel.angleDelta.y > 0 ? 1 : -1;
                        if (wheel.modifiers & Qt.ShiftModifier)
                            backend.adjust("rotation", step * 2);
                        else
                            backend.multiply("scale", step > 0 ? 1.03 : 1 / 1.03);
                    }
                }

                // The brush's size, where it would paint.
                Rectangle {
                    visible: backend.tool === "brush" && canvasMouse.containsMouse
                    readonly property real r: backend.brushRadius * view.zoomFactor
                    width: 2 * r
                    height: 2 * r
                    radius: r
                    x: canvasMouse.mouseX - r
                    y: canvasMouse.mouseY - r
                    color: "transparent"
                    border.color: theme.accent
                    border.width: 1.5
                }

                // The AI at work.
                Rectangle {
                    anchors.centerIn: parent
                    visible: backend.busy !== ""
                    radius: 8
                    color: "#e0141417"
                    border.color: theme.accent
                    width: busyRow.implicitWidth + 32
                    height: busyRow.implicitHeight + 24
                    Row {
                        id: busyRow
                        anchors.centerIn: parent
                        spacing: 12
                        BusyIndicator { running: parent.parent.visible; width: 28; height: 28 }
                        Label { anchors.verticalCenter: parent.verticalCenter; text: backend.busy; color: win.text; font.pixelSize: 15 }
                    }
                }

                // What the current tool wants next.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 14
                    visible: backend.hasProject && (backend.tool !== "move" || backend.handle >= 0)
                    radius: 6
                    color: "#e0141417"
                    border.color: theme.accent
                    width: Math.min(toolHint.implicitWidth + 28, parent.width - 24)
                    height: toolHint.height + 14
                    Label {
                        id: toolHint
                        anchors.centerIn: parent
                        width: parent.width - 28
                        wrapMode: Text.WordWrap
                        color: win.text
                        horizontalAlignment: Text.AlignHCenter
                        lineHeight: 1.3
                        text: backend.handle >= 0
                              ? (backend.handle >= 4 ? "Turning: " + ["yaw", "pitch", "roll"][backend.handle - 4] : backend.handle === 0 ? "Moving" : "Moving along " + ["", "x", "y", "z"][backend.handle])
                                + "   ·   X, Y, Z keep to an axis (again frees it)   ·   Ctrl snaps   ·   click or Enter to place   ·   right-click or Esc puts it back"
                              : backend.tool === "pin"
                              ? "Pins: click the layer to pin it, drag a pin to bend it, right-click one to take it away   ·   K keyframes the bend   ·   Esc done"
                              : backend.tool === "brush"
                              ? "Paint the shape: drag to add, right-drag to take away   ·   wheel: brush size   ·   Enter apply   ·   Esc cancel"
                              : backend.tool === "track"
                              ? "Track: click a point on " + (backend.values.name || "the video") + " with some detail around it; it's followed from the playhead to the end, and an empty keyed there   ·   Esc cancel"
                              : backend.tool === "draw"
                              ? "Draw on " + (backend.values.name || "the drawing") + ": drag to draw, right-click takes back a stroke, the wheel sizes the pen ("
                                + Math.round(backend.brushRadius / 4 * 10) / 10 + " px)   ·   , . to another frame: drawing there starts a new drawing   ·   Esc done"
                              : backend.tool === "path"
                              ? "Path for " + (backend.values.name || "the layer") + ": click points, or drag to draw it (" + backend.cutPoints.length + " so far)"
                                + "\nEnter: it follows them from the playhead to the end   ·   Shift+Enter: turning with the way   ·   Backspace drops a point   ·   Esc cancel"
                              : backend.tool === "pivot"
                              ? "Pivot: click where the layer should turn and scale around   ·   Esc cancel"
                              : backend.tool === "wand" && backend.wandMode === "ai"
                              ? "AI select: click a thing on " + (backend.values.name || "the layer") + " (Shift+click adds, right-click leaves out)"
                                + (backend.aiChoices > 1 ? "   ·   T other guesses (" + (backend.aiChoice + 1) + " of " + backend.aiChoices + ")" : "")
                                + "   ·   M colour select"
                                + "\nC fine-tune the points   ·   F soft edge: " + backend.feather + " px   ·   G fill the hole: " + backend.fillMode
                                + "   ·   Enter cut out   ·   Shift+Enter copy out   ·   Esc cancel"
                              : backend.tool === "wand"
                              ? "Colour select: click a part of " + (backend.values.name || "the layer") + " (Shift+click adds)   ·   M AI select   ·   wheel or T: tolerance "
                                + (backend.tolerance < 0 ? (backend.cutPoints.length ? "auto, " + backend.usedTolerance : "auto") : backend.tolerance)
                                + "\nC fine-tune the points   ·   F soft edge: " + backend.feather + " px   ·   G fill the hole: " + backend.fillMode
                                + "   ·   Enter cut out   ·   Shift+Enter copy out   ·   Esc cancel"
                              : (backend.reshaping !== ""
                                 ? "Reshape " + backend.reshaping + ": drag points, click an edge to add one, right-click one to remove it"
                                 : "Cut: drag around the piece, or click point by point (" + backend.cutPoints.length + " so far)")
                                + "\nF soft edge: " + backend.feather + " px   ·   G fill the hole: " + backend.fillMode + "   ·   "
                                + (backend.reshaping !== "" ? "Enter apply" : "Enter cut out   ·   Shift+Enter copy out")
                                + "   ·   Backspace drops the last point   ·   Esc cancel"
                    }
                }

                // Nothing open yet.
                Column {
                    anchors.centerIn: parent
                    visible: !backend.hasProject
                    spacing: 18

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "omotion"
                        font.pixelSize: 44
                        font.bold: true
                        color: theme.accent
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Cut a picture into layers and bring it to life."
                        color: win.muted
                        font.pixelSize: 16
                    }
                    Item { width: 1; height: 8 }
                    Repeater {
                        model: [["O", "Animate an image", function () { backend.openImageDialog() }],
                                ["Shift+O", "Open an animation folder", function () { backend.openFolderDialog() }],
                                ["?", "Every shortcut", function () { win.helpVisible = true }]]
                        delegate: Row {
                            required property var modelData
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 14
                            Rectangle {
                                width: 76; height: 30; radius: 5
                                color: "#1d1d21"; border.color: win.line
                                Label { anchors.centerIn: parent; text: modelData[0]; color: theme.accent; font.bold: true }
                            }
                            Label {
                                width: 220
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData[1]
                                color: win.text
                                font.pixelSize: 15
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: modelData[2]() }
                            }
                        }
                    }
                }
            }

            Rectangle { Layout.fillHeight: true; width: 1; color: win.line; visible: backend.hasProject }

            // The outliner over the selected thing's properties, as in Blender.
            ColumnLayout {
                Layout.preferredWidth: 270
                Layout.maximumWidth: 270
                Layout.fillWidth: false
                Layout.fillHeight: true
                visible: backend.hasProject
                spacing: 0
                Outliner {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(240, 34 + backend.outliner.length * 24)
                    onDone: win.focusCanvas()
                    onRename: function (index) { timeline.rename(index) }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: win.line }
                Inspector {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onDone: win.focusCanvas()
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: win.line; visible: backend.hasProject }

        Timeline {
            id: timeline
            Layout.fillWidth: true
            // Curves want more room than rows of keys.
            Layout.preferredHeight: curves ? 320 : 200
            visible: backend.hasProject
            onDone: win.focusCanvas()
        }
    }

    // ---- Overlays --------------------------------------------------------

    Rectangle {
        anchors.fill: parent
        visible: win.exportMenuVisible
        color: "#b0000000"
        MouseArea { anchors.fill: parent; onClicked: win.exportMenuVisible = false }

        Shortcut { sequences: ["1", "M"]; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: exportChoice.pick("mp4") }
        Shortcut { sequences: ["2", "W"]; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: exportChoice.pick("webm") }
        Shortcut { sequences: ["3", "G"]; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: exportChoice.pick("gif") }
        Shortcut { sequences: ["4", "F"]; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: exportChoice.pick("png") }
        Shortcut { sequences: ["5"]; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: exportChoice.pick("glb") }
        Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: { win.exportMenuVisible = false; backend.openRenders() } }
        Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: win.exportHalf = !win.exportHalf }

        Rectangle {
            id: exportChoice
            anchors.centerIn: parent
            width: 440
            height: exportColumn.implicitHeight + 40
            radius: 10
            color: win.panel
            border.color: win.line

            function pick(format) {
                win.exportMenuVisible = false;
                backend.exportAs(format, win.exportHalf ? 0.5 : 1);
            }

            Column {
                id: exportColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 6
                Label { text: "Export"; font.pixelSize: 20; font.bold: true; color: win.text }
                Label {
                    readonly property real factor: win.exportHalf ? 0.5 : 1
                    text: "Into renders/ in the project folder. "
                          + win.evenSize(backend.sceneInfo.width * factor) + "×" + win.evenSize(backend.sceneInfo.height * factor)
                          + (win.exportHalf ? " (half size)" : "") + ", " + backend.sceneInfo.fps + " fps, " + backend.sceneInfo.frames + " frames."
                    color: win.muted
                    wrapMode: Text.Wrap
                    width: parent.width
                }
                Item { width: 1; height: 6 }
                Repeater {
                    model: [["1", "MP4", "H.264, plays everywhere", "mp4"],
                            ["2", "WebM", "VP9, keeps transparency", "webm"],
                            ["3", "GIF", "Loops at 15 fps, for chats and READMEs", "gif"],
                            ["4", "PNG frames", "A numbered image per frame", "png"],
                            ["5", "glTF", "The scene in 3D, moving, for Blender", "glb"]]
                    delegate: Rectangle {
                        required property var modelData
                        width: exportColumn.width
                        height: 44
                        radius: 6
                        color: rowMouse.containsMouse ? "#1f1f24" : "transparent"
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            spacing: 14
                            Label { text: modelData[0]; color: theme.accent; font.bold: true; width: 14 }
                            Label { text: modelData[1]; color: win.text; font.bold: true; width: 100 }
                            Label { text: modelData[2]; color: win.muted }
                        }
                        MouseArea { id: rowMouse; anchors.fill: parent; hoverEnabled: true; onClicked: exportChoice.pick(modelData[3]) }
                    }
                }
                Item { width: 1; height: 4 }
                Label { text: "H " + (win.exportHalf ? "full size" : "half size") + "   ·   R opens the renders folder   ·   Esc closes"; color: win.muted }
            }
        }
    }

    Rectangle {
        id: presetMenu
        anchors.fill: parent
        visible: win.presetMenuVisible
        color: "#b0000000"
        MouseArea { anchors.fill: parent; onClicked: win.presetMenuVisible = false }

        // Read again each time it opens, as motions are saved.
        property var saved: []
        onVisibleChanged: if (visible) saved = backend.savedMotions()
        Repeater {
            model: 3
            delegate: Item {
                required property int index
                Shortcut {
                    sequence: String(index + 7)
                    context: Qt.ApplicationShortcut
                    enabled: win.presetMenuVisible && index < presetMenu.saved.length
                    onActivated: { win.presetMenuVisible = false; backend.applyMotion(presetMenu.saved[index]) }
                }
            }
        }
        readonly property var choices: [["1", "Sway", "A gentle rock, for trees, grass and signs", "sway"],
                                        ["2", "Float", "Drifting up and down", "float"],
                                        ["3", "Pulse", "Breathing in and out", "pulse"],
                                        ["4", "Bounce", "Hopping", "bounce"],
                                        ["5", "Spin", "One turn over the scene", "spin"],
                                        ["6", "Fade in", "From the playhead, over a second", "fade"]]
        Repeater {
            model: 6
            delegate: Item {
                required property int index
                Shortcut {
                    sequence: String(index + 1)
                    context: Qt.ApplicationShortcut
                    enabled: win.presetMenuVisible
                    onActivated: presetChoice.pick(presetMenu.choices[index][3])
                }
            }
        }

        Rectangle {
            id: presetChoice
            anchors.centerIn: parent
            width: 460
            height: presetColumn.implicitHeight + 40
            radius: 10
            color: win.panel
            border.color: win.line

            function pick(preset) {
                win.presetMenuVisible = false;
                backend.applyPreset(preset);
            }

            Column {
                id: presetColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 6
                Label { text: "Move " + (backend.values.name || ""); font.pixelSize: 20; font.bold: true; color: win.text }
                Label {
                    text: "A ready-made motion; all but the fade loop with the scene. It replaces that property's keyframes; Ctrl+Z takes it back."
                    color: win.muted
                    wrapMode: Text.Wrap
                    width: parent.width
                }
                Item { width: 1; height: 6 }
                Repeater {
                    model: presetMenu.choices
                    delegate: Rectangle {
                        required property var modelData
                        width: presetColumn.width
                        height: 40
                        radius: 6
                        color: presetMouse.containsMouse ? "#1f1f24" : "transparent"
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            spacing: 14
                            Label { text: modelData[0]; color: theme.accent; font.bold: true; width: 14 }
                            Label { text: modelData[1]; color: win.text; font.bold: true; width: 80 }
                            Label { text: modelData[2]; color: win.muted }
                        }
                        MouseArea { id: presetMouse; anchors.fill: parent; hoverEnabled: true; onClicked: presetChoice.pick(modelData[3]) }
                    }
                }
                Label {
                    visible: presetMenu.saved.length > 0
                    text: "Saved"
                    color: win.text
                    font.bold: true
                    topPadding: 8
                }
                Repeater {
                    model: presetMenu.saved
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: presetColumn.width
                        height: 36
                        radius: 6
                        color: savedMouse.containsMouse ? "#1f1f24" : "transparent"
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            spacing: 14
                            Label { text: index < 3 ? String(index + 7) : ""; color: theme.accent; font.bold: true; width: 14 }
                            Label { text: modelData; color: win.text; font.bold: true }
                        }
                        // As a clip: on top of its animation, its keys untouched.
                        ToolButton {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "as a clip"
                            font.pixelSize: 12
                            z: 1
                            onClicked: { win.presetMenuVisible = false; backend.addClip(modelData, 1) }
                        }
                        MouseArea {
                            id: savedMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { win.presetMenuVisible = false; backend.applyMotion(modelData) }
                        }
                    }
                }
                Item { width: 1; height: 4 }
                Label { text: "Ctrl+Shift+S saves a layer's animation here   ·   Esc closes"; color: win.muted }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: win.saveMotionVisible
        color: "#b0000000"
        MouseArea { anchors.fill: parent; onClicked: win.saveMotionVisible = false }
        onVisibleChanged: if (visible) { motionName.text = ""; motionName.forceActiveFocus() }
        Rectangle {
            anchors.centerIn: parent
            width: 440
            height: saveColumn.implicitHeight + 40
            radius: 10
            color: win.panel
            border.color: win.line
            Column {
                id: saveColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 10
                Label { text: "Save the motion"; font.pixelSize: 20; font.bold: true; color: win.text }
                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: win.muted
                    text: "The " + (backend.keySelection.length ? "picked keyframes" : "animation of " + (backend.values.name || "the layer"))
                          + ", to put on any layer in any project from N."
                }
                TextField {
                    id: motionName
                    width: parent.width
                    placeholderText: "A name, like wave hello"
                    onAccepted: {
                        if (text.trim() === "") return;
                        backend.saveMotion(text);
                        win.saveMotionVisible = false;
                        win.focusCanvas();
                    }
                }
                Label { text: "Enter saves   ·   Esc closes"; color: win.muted }
            }
        }
    }

    Rectangle {
        id: particleMenu
        anchors.fill: parent
        visible: win.particleMenuVisible
        color: "#b0000000"
        MouseArea { anchors.fill: parent; onClicked: win.particleMenuVisible = false }
        readonly property var choices: [["1", "Snow", "Drifting down", "snow"], ["2", "Rain", "Slanting streaks", "rain"],
                                        ["3", "Dust", "Motes in the light", "dust"], ["4", "Fireflies", "Glowing, wandering", "fireflies"],
                                        ["5", "Leaves", "Tumbling down", "leaves"], ["6", "Embers", "Rising sparks", "embers"]]
        Repeater {
            model: 6
            delegate: Item {
                required property int index
                Shortcut {
                    sequence: String(index + 1)
                    context: Qt.ApplicationShortcut
                    enabled: win.particleMenuVisible
                    onActivated: particleChoice.pick(particleMenu.choices[index][3])
                }
            }
        }
        Rectangle {
            id: particleChoice
            anchors.centerIn: parent
            width: 440
            height: particleColumn.implicitHeight + 40
            radius: 10
            color: win.panel
            border.color: win.line
            function pick(kind) {
                win.particleMenuVisible = false;
                backend.addParticles(kind);
            }
            Column {
                id: particleColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 6
                Label { text: "Add particles"; font.pixelSize: 20; font.bold: true; color: win.text }
                Label {
                    text: "A layer of them on top, looping with the scene. Shape them in the inspector."
                    color: win.muted
                    wrapMode: Text.Wrap
                    width: parent.width
                }
                Item { width: 1; height: 6 }
                Repeater {
                    model: particleMenu.choices
                    delegate: Rectangle {
                        required property var modelData
                        width: particleColumn.width
                        height: 40
                        radius: 6
                        color: particleMouse.containsMouse ? "#1f1f24" : "transparent"
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            spacing: 14
                            Label { text: modelData[0]; color: theme.accent; font.bold: true; width: 14 }
                            Label { text: modelData[1]; color: win.text; font.bold: true; width: 90 }
                            Label { text: modelData[2]; color: win.muted }
                        }
                        MouseArea { id: particleMouse; anchors.fill: parent; hoverEnabled: true; onClicked: particleChoice.pick(modelData[3]) }
                    }
                }
                Item { width: 1; height: 4 }
                Label { text: "Esc closes"; color: win.muted }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: win.quitConfirmVisible
        color: "#c0000000"
        MouseArea { anchors.fill: parent }
        Column {
            anchors.centerIn: parent
            spacing: 12
            Label { anchors.horizontalCenter: parent.horizontalCenter; text: "An export is still running."; font.pixelSize: 20; color: win.text }
            Label { anchors.horizontalCenter: parent.horizontalCenter; text: "Q quits and cancels it   ·   Esc keeps going"; color: win.muted }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: backend.aiPrompt !== ""
        color: "#c0000000"
        MouseArea { anchors.fill: parent; onClicked: backend.dismissAiPrompt() }
        Rectangle {
            anchors.centerIn: parent
            width: 520
            height: promptColumn.implicitHeight + 44
            radius: 10
            color: win.panel
            border.color: win.line
            Column {
                id: promptColumn
                anchors.fill: parent
                anchors.margins: 22
                spacing: 10
                Label {
                    text: backend.aiPrompt === "download" ? "Download the AI models?" : "Install ONNX Runtime?"
                    font.pixelSize: 20
                    font.bold: true
                    color: win.text
                }
                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: win.muted
                    text: backend.aiPrompt === "download"
                          ? "omotion's AI finds depth (Depth Anything V2 Small, Apache-2.0), selects things (MobileSAM, MIT), "
                            + "and paints in behind what you cut out (LaMa, Apache-2.0). "
                            + "Their models are about 350 MB, from Hugging Face, checked against known hashes, and kept in ~/.local/share/omotion/models. "
                            + "Everything runs on this computer."
                          : "omotion's AI runs on ONNX Runtime (onnxruntime-cpu, from the Arch repositories). "
                            + "It installs in a terminal, where you can enter your password."
                }
                Label { text: "Enter " + (backend.aiPrompt === "download" ? "downloads" : "installs") + "   ·   Esc not now"; color: theme.accent }
            }
        }
    }

    // The film this project is a shot in: its shots, how they join, export.
    Rectangle {
        anchors.fill: parent
        visible: win.filmVisible
        color: "#b0000000"
        onVisibleChanged: visible ? backend.showFilmAt(backend.filmTime) : backend.stopFilmPreview()
        MouseArea { anchors.fill: parent; onClicked: win.filmVisible = false }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(640, parent.width - 40)
            height: filmColumn.implicitHeight + 40
            radius: 10
            color: win.panel
            border.color: win.line
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: filmColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 6
                Label { text: "Film"; font.pixelSize: 20; font.bold: true; color: win.text }
                // The film as it will export, played here.
                Rectangle {
                    visible: backend.filmShots.length > 0
                    Layout.fillWidth: true
                    // Short enough that the shots below still fit the window.
                    Layout.preferredHeight: Math.min(width * 9 / 16, win.height * .3)
                    color: "black"
                    radius: 4
                    Image {
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        source: backend.filmPicture > 0 ? "image://film/" + backend.filmPicture : ""
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: backend.filmPicture === 0
                        text: "Drawing the film…"
                        color: win.muted
                    }
                    MouseArea { anchors.fill: parent; onClicked: backend.toggleFilmPlay() }
                }
                RowLayout {
                    visible: backend.filmShots.length > 0
                    Layout.fillWidth: true
                    spacing: 8
                    ToolButton {
                        text: backend.filmPlaying ? "❚❚" : "▶"
                        onClicked: backend.toggleFilmPlay()
                        ToolTip.visible: hovered
                        ToolTip.text: "Play or pause the film (Space)"
                    }
                    Slider {
                        id: filmScrub
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(.01, backend.filmDuration)
                        value: backend.filmTime
                        onMoved: { backend.stopFilmPreview(); backend.showFilmAt(value) }
                    }
                    Label {
                        text: backend.filmTime.toFixed(1) + " / " + backend.filmDuration.toFixed(1) + " s"
                        color: win.muted
                        font.pixelSize: 12
                    }
                }
                Connections {
                    target: backend
                    // Shots changed while it's open: shown again as they are now.
                    function onFilmChanged() { if (win.filmVisible && !backend.filmPlaying) backend.showFilmAt(backend.filmTime) }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    text: backend.filmShots.length
                          ? "Shots in a row, from " + backend.filmPath + ". Each joins the one before by a cut, a dissolve, a fade through black or a wipe."
                          : "No film here yet: add this project as its first shot, then open the others and add them."
                }
                Repeater {
                    model: backend.filmShots
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 6
                        Label { text: (index + 1) + "."; color: win.muted; Layout.preferredWidth: 22 }
                        Label {
                            text: String(modelData.project).replace(/\.omotion$/, "")
                            color: modelData.this ? theme.accent : win.text
                            font.bold: modelData.this
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        ComboBox {
                            visible: index > 0
                            font.pixelSize: 12
                            implicitHeight: 34
                            Layout.preferredWidth: 110
                            model: ["cut", "dissolve", "fade", "wipe"]
                            currentIndex: Math.max(0, model.indexOf(modelData.transition || "cut"))
                            onActivated: function (i) { backend.setShot(index, "transition", model[i]) }
                        }
                        TextField {
                            visible: index > 0 && (modelData.transition || "cut") !== "cut"
                            implicitHeight: 30
                            Layout.preferredWidth: 56
                            font.pixelSize: 12
                            text: String(modelData.length === undefined ? 0.5 : modelData.length)
                            onAccepted: backend.setShot(index, "length", Number(text))
                            ToolTip.visible: hovered
                            ToolTip.text: "Seconds the transition takes"
                        }
                        ToolButton { text: "↑"; enabled: index > 0; onClicked: backend.moveShot(index, -1) }
                        ToolButton { text: "↓"; enabled: index + 1 < backend.filmShots.length; onClicked: backend.moveShot(index, 1) }
                        ToolButton { text: "✕"; onClicked: backend.removeShot(index) }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    ToolButton { text: "Add this shot"; onClicked: backend.addToFilm() }
                    Item { Layout.fillWidth: true }
                    ToolButton {
                        text: "Export the film"
                        enabled: backend.filmShots.length > 0 && !backend.exporting
                        onClicked: { win.filmVisible = false; backend.exportFilm() }
                    }
                }
                Label { text: "Space plays   ·   Ctrl+Shift+F or Esc closes   ·   omotion film renders it from the command line"; color: win.muted }
            }
        }
    }

    // What the agent did, to keep or take back.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 230
        visible: backend.agentReview
        width: Math.min(640, parent.width - 40)
        height: reviewColumn.implicitHeight + 28
        radius: 10
        color: "#f0141417"
        border.color: theme.accent
        ColumnLayout {
            id: reviewColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: backend.agentReply
                color: win.text
                font.pixelSize: 15
            }
            Repeater {
                model: backend.agentChanges
                delegate: Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "·  " + modelData
                    color: win.muted
                    font.pixelSize: 12
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Space plays it   ·   Enter keeps   ·   Esc takes it back"; color: theme.accent; Layout.fillWidth: true }
                ToolButton { text: "Keep"; onClicked: { backend.keepAgent(); win.focusCanvas() } }
                ToolButton { text: "Take back"; onClicked: { backend.takeBackAgent(); win.focusCanvas() } }
            }
        }
    }

    CommandBox {
        id: commandBox
        anchors.fill: parent
        onClosed: win.focusCanvas()
    }

    HelpOverlay {
        anchors.fill: parent
        visible: win.helpVisible
    }
}
