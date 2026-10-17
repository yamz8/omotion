import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omotion

// The layer stack, top layer first, with each layer's keyframes along time.
Rectangle {
    id: timeline
    color: win.panel

    signal done()

    readonly property var layers: backend.layers
    readonly property real duration: backend.sceneInfo.duration || 1
    readonly property int nameWidth: 230
    property int renaming: -1
    // The graph editor in place of the tracks: the selected thing's curves.
    property bool curves: false
    // The dope sheet: the selected row opened into a row a property.
    property bool sheet: false

    function rename(index) { renaming = index; }
    function picked(layer, t) {
        const selection = backend.keySelection;
        for (let i = 0; i < selection.length; ++i)
            if (selection[i].layer === layer && Math.abs(selection[i].t - t) < 1e-6)
                return true;
        return false;
    }
    function format(t) { return Number(t).toFixed(2); }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Transport.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 38
            Layout.leftMargin: 8
            Layout.rightMargin: 14
            spacing: 10

            ToolButton {
                text: backend.playing ? "❚❚" : "▶"
                onClicked: { backend.togglePlay(); timeline.done() }
            }
            ToolButton {
                text: timeline.curves ? "Keys" : "Curves"
                onClicked: { timeline.curves = !timeline.curves; timeline.done() }
                ToolTip.visible: hovered
                ToolTip.text: "Ctrl+Tab: the graph editor, the selected layer's properties as curves"
            }
            Label {
                text: timeline.format(backend.time) + " / " + timeline.format(timeline.duration) + " s"
                color: win.text
                font.family: "monospace"
            }
            Label {
                text: "frame " + (backend.frame + 1) + " / " + (backend.sceneInfo.frames || 0)
                color: win.muted
                font.family: "monospace"
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideLeft
                text: timeline.curves
                      ? "drag a key in time and value (Shift: value only), its curve's handles to ease it    click a property to see it alone    Ctrl+Tab keys"
                      : "drag ◆ to retime    ▸ a row a property    , .  frame    < >  keyframe    K key    E ease    ?  all keys"
                color: win.muted
            }
        }

        // Ruler.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 22
            spacing: 0
            Label {
                Layout.preferredWidth: timeline.nameWidth
                leftPadding: 12
                text: timeline.curves ? "CURVES" : "LAYERS"
                color: win.muted
                font.pixelSize: 11
                font.letterSpacing: 1
            }
            Item {
                id: ruler
                objectName: "ruler"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.rightMargin: 14

                // A tick every few seconds, as many as fit with room for labels.
                readonly property real step: {
                    const steps = [1, 2, 5, 10, 15, 30, 60];
                    for (let i = 0; i < steps.length; ++i)
                        if (steps[i] / timeline.duration * width >= 60)
                            return steps[i];
                    return 120;
                }
                Repeater {
                    model: Math.floor(timeline.duration / ruler.step) + 1
                    delegate: Item {
                        required property int index
                        readonly property real seconds: index * ruler.step
                        x: seconds / timeline.duration * ruler.width
                        height: ruler.height
                        Rectangle { width: 1; height: 8; anchors.bottom: parent.bottom; color: win.muted }
                        Label {
                            x: 4
                            text: parent.seconds + "s"
                            color: win.muted
                            font.pixelSize: 11
                            visible: parent.seconds < timeline.duration
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    function scrub(x) { backend.seek(Math.max(0, x) / ruler.width * timeline.duration) }
                    onPressed: function (mouse) { if (backend.playing) backend.togglePlay(); scrub(mouse.x) }
                    onPositionChanged: function (mouse) { scrub(mouse.x) }
                    onReleased: timeline.done()
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: win.line }

        // The sound, as its waveform along the scene, to time things to.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            Layout.fillHeight: false
            visible: !!backend.audio.source
            spacing: 0
            Label {
                Layout.preferredWidth: timeline.nameWidth
                leftPadding: 12
                text: "♪  " + (backend.audio.name || "")
                color: "#c4c4cc"
                elide: Text.ElideRight
            }
            Canvas {
                id: wave
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.rightMargin: 14
                readonly property var peaks: backend.audio.peaks || []
                onPeaksChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    const n = peaks.length;
                    if (!n)
                        return;
                    ctx.fillStyle = String(theme.accent);
                    ctx.globalAlpha = .55;
                    const w = width / n, mid = height / 2;
                    for (let i = 0; i < n; ++i) {
                        const h = Math.max(1, peaks[i] * (height - 4));
                        ctx.fillRect(i * w, mid - h / 2, Math.max(1, w - .5), h);
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    function scrub(x) { backend.seek(Math.max(0, x) / wave.width * timeline.duration) }
                    onPressed: function (mouse) { if (backend.playing) backend.togglePlay(); scrub(mouse.x) }
                    onPositionChanged: function (mouse) { scrub(mouse.x) }
                    onReleased: timeline.done()
                }
            }
        }

        // The graph editor: the properties beside their curves.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: timeline.curves
            spacing: 0
            ListView {
                id: curveNames
                Layout.preferredWidth: timeline.nameWidth
                Layout.fillHeight: true
                clip: true
                model: backend.curveNames
                boundsBehavior: Flickable.StopAtBounds
                header: Label {
                    width: curveNames.width
                    leftPadding: 12
                    topPadding: 4
                    bottomPadding: 2
                    text: (backend.cameraSelected ? "Camera" : backend.values.name || "") + (graph.solo ? "" : ", all fitted")
                    color: win.muted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                delegate: Rectangle {
                    required property string modelData
                    width: curveNames.width
                    height: 22
                    color: graph.solo === modelData ? "#1e2130" : "transparent"
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 12
                        spacing: 8
                        Rectangle { width: 10; height: 3; radius: 1; anchors.verticalCenter: parent.verticalCenter; color: graph.colour(modelData) }
                        Label { text: modelData; color: graph.solo === "" || graph.solo === modelData ? win.text : win.muted }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: { graph.solo = graph.solo === modelData ? "" : modelData; timeline.done() }
                    }
                }
            }
            // Curve modifiers for the property seen alone, as Blender's.
            ColumnLayout {
                visible: graph.solo !== ""
                Layout.preferredWidth: 150
                Layout.fillHeight: true
                spacing: 0
                readonly property var mods: (backend.values.modifiers || {})[graph.solo] || []
                function has(type) {
                    for (let i = 0; i < mods.length; ++i)
                        if (mods[i].type === type) return true;
                    return false;
                }
                // A wobble about the size this property moves by.
                function noiseAmount(p) {
                    return ({"x": 8, "y": 8, "rotation": 3, "scale": .03, "zoom": .03, "opacity": .1,
                             "worldX": .05, "worldY": .05, "worldZ": .05, "yaw": 2, "pitch": 2, "roll": 2})[p] || 1;
                }
                Label { text: "Modifiers"; color: win.muted; font.pixelSize: 11; topPadding: 6 }
                CheckBox {
                    text: "Cycle"
                    checked: parent.has("cycle")
                    onClicked: { backend.setModifier(graph.solo, checked ? "cycle" : "none"); timeline.done() }
                    ToolTip.visible: hovered
                    ToolTip.text: "Repeat its keys over and over, past the last one"
                }
                CheckBox {
                    text: "Noise"
                    checked: parent.has("noise")
                    onClicked: { backend.setModifier(graph.solo, checked ? "noise" : "none", parent.noiseAmount(graph.solo), 2); timeline.done() }
                    ToolTip.visible: hovered
                    ToolTip.text: "A smooth wobble on top: a hand-held camera, a shake, a flicker"
                }
            }
            GraphView {
                id: graph
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.rightMargin: 14
                editor: backend
                accent: theme.accent
                // A property no longer animated (or on another layer) isn't soloed.
                Connections {
                    target: backend
                    function onValuesChanged() { if (graph.solo !== "" && backend.curveNames.indexOf(graph.solo) < 0) graph.solo = "" }
                }
                MouseArea {
                    anchors.fill: parent
                    onPressed: function (mouse) { graph.press(mouse.x, mouse.y) }
                    onPositionChanged: function (mouse) { graph.drag(mouse.x, mouse.y, (mouse.modifiers & Qt.ShiftModifier) !== 0) }
                    onReleased: { graph.release(); timeline.done() }
                }
            }
        }

        ListView {
            id: rows
            visible: !timeline.curves
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // The camera's row first, then the layers, top one first.
            model: timeline.layers.length + 1
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Column {
                id: rowGroup
                required property int index
                width: rows.width
                Rectangle {
                    id: row
                    readonly property int index: rowGroup.index
                    readonly property bool isCamera: index === 0
                    readonly property int layerIndex: isCamera ? -2 : timeline.layers.length - index
                    readonly property var entry: isCamera
                        ? {"name": "Camera", "visible": true, "depth": 0, "parent": "", "keyTimes": backend.sceneInfo.cameraKeys || []}
                        : (timeline.layers[layerIndex] || {})
                    readonly property bool selected: isCamera ? backend.cameraSelected : backend.selected === layerIndex
                    width: rows.width
                    height: 26
                    color: selected ? "#1e2130" : (index % 2 ? "#121215" : "transparent")

                    Rectangle { visible: row.selected; width: 3; height: parent.height; color: theme.accent }

                    RowLayout {
                        anchors.fill: parent
                        spacing: 0

                        // Name column.
                        Item {
                            Layout.preferredWidth: timeline.nameWidth
                            Layout.fillHeight: true

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    if (row.isCamera) backend.selectCamera();
                                    else backend.select(row.layerIndex);
                                    timeline.done();
                                }
                                onDoubleClicked: if (!row.isCamera) timeline.rename(row.layerIndex)
                            }
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                x: 10
                                spacing: 6
                                Label {
                                    text: row.isCamera ? "◎" : row.entry.visible ? "●" : "○"
                                    color: row.entry.visible ? win.text : win.muted
                                    width: 14
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: { if (!row.isCamera) backend.toggleVisible(row.layerIndex); timeline.done() }
                                    }
                                }
                                // Opens the dope sheet: a row for each property.
                                Label {
                                    text: row.selected && timeline.sheet ? "▾" : "▸"
                                    color: (row.entry.keyTimes || []).length ? win.muted : "transparent"
                                    width: 10
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        enabled: (row.entry.keyTimes || []).length > 0
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (!row.selected) {
                                                if (row.isCamera) backend.selectCamera();
                                                else backend.select(row.layerIndex);
                                                timeline.sheet = true;
                                            } else
                                                timeline.sheet = !timeline.sheet;
                                            timeline.done();
                                        }
                                    }
                                }
                                Item { width: (row.entry.depth || 0) * 12; height: 1 }
                                Label {
                                    visible: !!row.entry.parent
                                    text: "↳"
                                    color: win.muted
                                }
                                Label {
                                    visible: timeline.renaming !== row.layerIndex
                                    text: row.entry.name || ""
                                    color: row.entry.visible ? (row.selected ? win.text : "#c4c4cc") : win.muted
                                    font.bold: row.selected
                                    elide: Text.ElideRight
                                    width: timeline.nameWidth - 60 - (row.entry.depth || 0) * 12
                                }
                                TextField {
                                    id: renameField
                                    visible: timeline.renaming === row.layerIndex
                                    width: timeline.nameWidth - 60
                                    height: 26
                                    topPadding: 2
                                    bottomPadding: 2
                                    font.pixelSize: 13
                                    onVisibleChanged: if (visible) { text = row.entry.name; selectAll(); forceActiveFocus() }
                                    onAccepted: {
                                        backend.renameLayer(row.layerIndex, text);
                                        timeline.renaming = -1;
                                        timeline.done();
                                    }
                                    onActiveFocusChanged: if (!activeFocus && visible) timeline.renaming = -1
                                }
                            }
                        }

                        // Keyframe track.
                        Item {
                            id: track
                            objectName: "track"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.rightMargin: 14

                            // Pressing on a keyframe selects and drags it (Shift adds
                            // to the selection); anywhere else scrubs. This lives on
                            // the track rather than each diamond, because diamonds
                            // are rebuilt as keys move, which would end the drag.
                            MouseArea {
                                anchors.fill: parent
                                property bool draggingKeys: false
                                property real startX: 0
                                cursorShape: draggingKeys ? Qt.SizeHorCursor : Qt.ArrowCursor
                                function scrub(x) { backend.seek(Math.max(0, x) / track.width * timeline.duration) }
                                function keyNear(x) {
                                    const times = row.entry.keyTimes || [];
                                    let best = -1, bestDistance = 7;
                                    for (let i = 0; i < times.length; ++i) {
                                        const d = Math.abs(times[i] / timeline.duration * track.width - x);
                                        if (d <= bestDistance) { best = times[i]; bestDistance = d; }
                                    }
                                    return best;
                                }
                                onPressed: function (mouse) {
                                    if (backend.playing) backend.togglePlay();
                                    const shift = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                                    const t = keyNear(mouse.x);
                                    if (t >= 0) {
                                        backend.selectKey(row.layerIndex, t, shift);
                                        startX = mouse.x;
                                        draggingKeys = true;
                                        backend.beginKeyDrag();
                                        return;
                                    }
                                    if (!shift) backend.clearKeySelection();
                                    if (row.isCamera) backend.selectCamera();
                                    else backend.select(row.layerIndex);
                                    scrub(mouse.x);
                                }
                                onPositionChanged: function (mouse) {
                                    if (draggingKeys)
                                        backend.dragKeys((mouse.x - startX) / track.width * timeline.duration);
                                    else
                                        scrub(mouse.x);
                                }
                                onReleased: {
                                    if (draggingKeys) backend.endKeyDrag();
                                    draggingKeys = false;
                                    timeline.done();
                                }
                            }
                            Repeater {
                                model: row.entry.keyTimes || []
                                delegate: Rectangle {
                                    required property var modelData
                                    readonly property bool picked: timeline.picked(row.layerIndex, modelData)
                                    width: picked ? 11 : 9
                                    height: width
                                    rotation: 45
                                    x: modelData / timeline.duration * track.width - width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: picked ? "white" : (row.selected ? theme.accent : "#9a9aa4")
                                    border.color: picked ? theme.accent : "#0e0e10"
                                    border.width: picked ? 2 : 1
                                }
                            }
                        }
                    }
                }
                // The dope sheet's rows: each property's own keys, dragged
                // along in time on their own; right-click takes one away. The
                // mouse is caught over all of them at once, not on each row,
                // because the rows are made again as their keys move, which
                // would end the drag.
                Item {
                    id: sheetBlock
                    readonly property var sheet: row.selected && timeline.sheet ? backend.sheet : []
                    width: rows.width
                    height: sheet.length * 20
                    visible: sheet.length > 0
                    Column {
                        Repeater {
                            model: sheetBlock.sheet
                            delegate: Rectangle {
                                id: sheetRow
                                required property var modelData
                                width: rows.width
                                height: 20
                                color: "#16161a"
                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 0
                                    Label {
                                        Layout.preferredWidth: timeline.nameWidth
                                        leftPadding: 46
                                        text: sheetRow.modelData.property
                                        color: win.muted
                                        font.pixelSize: 12
                                    }
                                    Item {
                                        id: sheetTrack
                                        objectName: "sheetTrack"
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.rightMargin: 14
                                        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 1; color: "#26262c" }
                                        Repeater {
                                            model: sheetRow.modelData.times
                                            delegate: Rectangle {
                                                required property var modelData
                                                width: 7
                                                height: 7
                                                rotation: 45
                                                x: modelData / timeline.duration * sheetTrack.width - width / 2
                                                anchors.verticalCenter: parent.verticalCenter
                                                color: theme.accent
                                                border.color: "#0e0e10"
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    MouseArea {
                        x: timeline.nameWidth
                        width: parent.width - timeline.nameWidth - 14
                        height: parent.height
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        property string dragging: ""
                        property int key: -1
                        function keyNear(times, x) {
                            let best = -1, bestDistance = 7;
                            for (let i = 0; i < times.length; ++i) {
                                const d = Math.abs(times[i] / timeline.duration * width - x);
                                if (d <= bestDistance) { best = i; bestDistance = d; }
                            }
                            return best;
                        }
                        function timeAt(x) { return Math.max(0, Math.min(timeline.duration, x / width * timeline.duration)) }
                        onPressed: function (mouse) {
                            if (backend.playing) backend.togglePlay();
                            const line = sheetBlock.sheet[Math.floor(mouse.y / 20)];
                            if (!line) return;
                            const near = keyNear(line.times, mouse.x);
                            if (mouse.button === Qt.RightButton) {
                                if (near >= 0) backend.removeKey(line.property, near);
                                return;
                            }
                            dragging = near >= 0 ? line.property : "";
                            key = near;
                            if (near < 0) backend.seek(timeAt(mouse.x));
                        }
                        onPositionChanged: function (mouse) {
                            if (dragging !== "") backend.retimeKey(dragging, key, timeAt(mouse.x));
                            else backend.seek(timeAt(mouse.x));
                        }
                        onReleased: { dragging = ""; key = -1; timeline.done() }
                    }
                }
            }
        }
    }

    // Playhead, across the ruler and every row.
    Rectangle {
        x: timeline.nameWidth + backend.time / timeline.duration * (timeline.width - timeline.nameWidth - 14) - 1
        y: 38
        width: 2
        height: timeline.height - 38
        color: theme.accent
        Rectangle { width: 10; height: 10; radius: 5; x: -4; y: 0; color: theme.accent }
    }

    Label {
        anchors.centerIn: parent
        visible: timeline.layers.length === 0
        anchors.verticalCenterOffset: 30
        text: "No layers yet. Press I to add an image."
        color: win.muted
    }
}
