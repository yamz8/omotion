import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The layer stack, top layer first, with each layer's keyframes along time.
Rectangle {
    id: timeline
    color: win.panel

    signal done()

    readonly property var layers: backend.layers
    readonly property real duration: backend.sceneInfo.duration || 1
    readonly property int nameWidth: 230
    property int renaming: -1

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
                text: "drag ◆ to retime    , .  frame    < >  keyframe    K key    E ease    ?  all keys"
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
                text: "LAYERS"
                color: win.muted
                font.pixelSize: 11
                font.letterSpacing: 1
            }
            Item {
                id: ruler
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.rightMargin: 14

                Repeater {
                    model: Math.floor(timeline.duration) + 1
                    delegate: Item {
                        required property int index
                        x: index / timeline.duration * ruler.width
                        height: ruler.height
                        Rectangle { width: 1; height: 8; anchors.bottom: parent.bottom; color: win.muted }
                        Label {
                            x: 4
                            text: index + "s"
                            color: win.muted
                            font.pixelSize: 11
                            visible: index < timeline.duration
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

        ListView {
            id: rows
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: timeline.layers.length
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: row
                required property int index
                readonly property int layerIndex: timeline.layers.length - 1 - index
                readonly property var entry: timeline.layers[layerIndex] || {}
                readonly property bool selected: backend.selected === layerIndex
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
                            onClicked: { backend.select(row.layerIndex); timeline.done() }
                            onDoubleClicked: timeline.rename(row.layerIndex)
                        }
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            spacing: 6
                            Label {
                                text: row.entry.visible ? "●" : "○"
                                color: row.entry.visible ? win.text : win.muted
                                width: 14
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: { backend.toggleVisible(row.layerIndex); timeline.done() }
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
                                backend.select(row.layerIndex);
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
        text: "No layers yet. Press I to add an image."
        color: win.muted
    }
}
