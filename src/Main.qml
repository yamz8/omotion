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
    property bool quitConfirmVisible: false
    // Editing shortcuts go quiet while an overlay is up.
    readonly property bool ready: backend.hasProject && !helpVisible && !exportMenuVisible && !quitConfirmVisible
    readonly property bool hasLayer: ready && backend.selected >= 0

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

    onClosing: function (close) {
        if (backend.exporting && !quitConfirmVisible) {
            close.accepted = false;
            quitConfirmVisible = true;
        }
    }

    // ---- Shortcuts -------------------------------------------------------

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: { win.exportMenuVisible = false; win.helpVisible = !win.helpVisible } }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.helpVisible; onActivated: win.quit() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.quitConfirmVisible) win.quitConfirmVisible = false;
            else if (win.helpVisible) win.helpVisible = false;
            else if (win.exportMenuVisible) win.exportMenuVisible = false;
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
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.cycleBlend() }

    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("x", -1) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("x", 1) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("y", -1) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("y", 1) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("x", -10) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("x", 10) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("y", -10) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("y", 10) }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("rotation", -1) }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("rotation", 1) }
    Shortcut { sequence: "{"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("rotation", -15) }
    Shortcut { sequence: "}"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.adjust("rotation", 15) }
    Shortcut { sequence: "-"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.multiply("scale", 1 / 1.05) }
    Shortcut { sequences: ["=", "+"]; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.multiply("scale", 1.05) }
    Shortcut { sequence: "9"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setProperty("opacity", Math.max(0, backend.values.opacity - 0.1)) }
    Shortcut { sequence: "0"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.setProperty("opacity", Math.min(1, backend.values.opacity + 0.1)) }

    Shortcut { sequence: "K"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.keyAll() }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.removeKeys() }
    Shortcut { sequence: "E"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.cycleEase() }
    Shortcut { sequence: "Ctrl+C"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.copyKeys() }
    Shortcut { sequence: "Ctrl+V"; context: Qt.ApplicationShortcut; enabled: win.hasLayer; onActivated: backend.pasteKeys() }

    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.setTool(backend.tool === "cut" ? "move" : "cut") }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.ready; onActivated: backend.setTool(backend.tool === "pivot" ? "move" : "pivot") }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: win.hasLayer && backend.tool === "move"; onActivated: backend.startReshape() }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "cut"; onActivated: backend.cycleFeather() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "cut"; onActivated: backend.toggleFill() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "cut"; onActivated: backend.finishCut(true) }
    Shortcut { sequences: ["Shift+Return", "Shift+Enter"]; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "cut"; onActivated: backend.finishCut(false) }
    Shortcut { sequence: "Backspace"; context: Qt.ApplicationShortcut; enabled: win.ready && backend.tool === "cut"; onActivated: backend.removeCutPoint() }

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

                SceneView {
                    id: view
                    anchors.fill: parent
                    editor: backend
                    accent: theme.accent
                    visible: backend.hasProject
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: backend.hasProject
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: backend.tool === "move" ? (pressed && backend.selected >= 0 ? Qt.ClosedHandCursor : Qt.ArrowCursor) : Qt.CrossCursor
                    property bool dragging: false
                    // In the cut tool: the point being dragged, or a lasso
                    // being drawn, and where it last dropped a point.
                    property int cutPoint: -1
                    property bool lassoing: false
                    property point lassoAt

                    onPressed: function (mouse) {
                        win.focusCanvas();
                        const p = view.toScene(mouse.x, mouse.y);
                        if (backend.tool === "cut") {
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
                        if (backend.tool === "pivot") {
                            backend.setPivotAt(p.x, p.y);
                            return;
                        }
                        // Shift drags the selected layer even when another
                        // one is on top of it.
                        if (!(mouse.modifiers & Qt.ShiftModifier))
                            backend.selectAt(p.x, p.y);
                        if (backend.selected >= 0 && mouse.button === Qt.LeftButton) {
                            backend.beginDrag(p.x, p.y);
                            dragging = true;
                        }
                    }
                    onPositionChanged: function (mouse) {
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
                        if (dragging)
                            backend.endDrag();
                        dragging = false;
                        cutPoint = -1;
                        lassoing = false;
                    }
                    onWheel: function (wheel) {
                        if (backend.selected < 0)
                            return;
                        const step = wheel.angleDelta.y > 0 ? 1 : -1;
                        if (wheel.modifiers & Qt.ControlModifier)
                            backend.adjust("rotation", step * 2);
                        else
                            backend.multiply("scale", step > 0 ? 1.03 : 1 / 1.03);
                    }
                }

                // What the current tool wants next.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 14
                    visible: backend.hasProject && backend.tool !== "move"
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
                        text: backend.tool !== "cut"
                              ? "Pivot: click where the layer should turn and scale around   ·   Esc cancel"
                              : (backend.reshaping !== ""
                                 ? "Reshape " + backend.reshaping + ": drag points, click an edge to add one, right-click one to remove it"
                                 : "Cut: drag around the piece, or click point by point (" + backend.cutPoints.length + " so far)")
                                + "\nF soft edge: " + backend.feather + " px   ·   G fill the hole: " + (backend.fill ? "on" : "off") + "   ·   "
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

            Inspector {
                Layout.preferredWidth: 270
                Layout.fillHeight: true
                visible: backend.hasProject
                onDone: win.focusCanvas()
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: win.line; visible: backend.hasProject }

        Timeline {
            id: timeline
            Layout.fillWidth: true
            Layout.preferredHeight: 200
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
        Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: win.exportMenuVisible; onActivated: { win.exportMenuVisible = false; backend.openRenders() } }

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
                backend.exportAs(format);
            }

            Column {
                id: exportColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 6
                Label { text: "Export"; font.pixelSize: 20; font.bold: true; color: win.text }
                Label {
                    text: "Into renders/ in the project folder. " + backend.sceneInfo.width + "×" + backend.sceneInfo.height
                          + ", " + backend.sceneInfo.fps + " fps, " + backend.sceneInfo.frames + " frames."
                    color: win.muted
                    wrapMode: Text.Wrap
                    width: parent.width
                }
                Item { width: 1; height: 6 }
                Repeater {
                    model: [["1", "MP4", "H.264, plays everywhere", "mp4"],
                            ["2", "WebM", "VP9, keeps transparency", "webm"],
                            ["3", "GIF", "Loops, for chats and READMEs", "gif"],
                            ["4", "PNG frames", "A numbered image per frame", "png"]]
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
                Label { text: "R opens the renders folder   ·   Esc closes"; color: win.muted }
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

    HelpOverlay {
        anchors.fill: parent
        visible: win.helpVisible
    }
}
