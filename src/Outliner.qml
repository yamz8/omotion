import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Everything in the scene as a tree: the camera, the world with its pieces
// under what they were cut from, then the layers over it. Click to select,
// the dot hides and shows, double-click renames.
Rectangle {
    id: outliner
    color: win.panel

    signal done()
    signal rename(int index)

    readonly property var rows: backend.outliner

    // A mark for each kind of thing, so the tree reads at a glance.
    function mark(kind) {
        return {"camera": "◎", "world": "◍", "picture": "▦", "piece": "▱", "shape": "▲", "particles": "✦", "image": "▭", "empty": "✛"}[kind] || "·";
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            text: "Outliner"
            color: win.muted
            font.pixelSize: 11
            font.capitalization: Font.AllUppercase
            leftPadding: 12
            topPadding: 8
            bottomPadding: 4
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: outliner.rows
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property string kind: modelData.kind
                readonly property bool selected: kind === "camera" ? backend.cameraSelected
                                                 : kind === "world" ? backend.selected < 0 && !backend.cameraSelected
                                                 : backend.selected === modelData.index || backend.alsoSelected.indexOf(modelData.index) >= 0
                width: list.width
                height: 24
                color: selected ? "#1e2130" : "transparent"

                Rectangle { visible: row.selected; width: 3; height: parent.height; color: theme.accent }

                MouseArea {
                    anchors.fill: parent
                    onClicked: function (mouse) {
                        // Ctrl+click adds to the selection.
                        if ((mouse.modifiers & Qt.ControlModifier) && row.modelData.index >= 0) { backend.selectAlso(row.modelData.index); outliner.done(); return }
                        if (row.kind === "camera") backend.selectCamera();
                        else if (row.kind === "world") backend.select(-1);
                        else backend.select(row.modelData.index);
                        outliner.done();
                    }
                    onDoubleClicked: if (row.modelData.index >= 0) outliner.rename(row.modelData.index)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10 + row.modelData.depth * 14
                    anchors.rightMargin: 8
                    spacing: 6

                    Label {
                        text: outliner.mark(row.kind)
                        color: row.kind === "world" || row.kind === "camera" ? theme.accent : win.muted
                        Layout.preferredWidth: 14
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Label {
                        text: row.modelData.name
                        color: row.modelData.visible ? (row.selected ? win.text : "#c4c4cc") : win.muted
                        font.bold: row.selected || row.kind === "world"
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: row.modelData.detail || ""
                        color: win.muted
                        font.pixelSize: 11
                    }
                    Label {
                        visible: row.modelData.animated
                        text: "◆"
                        color: theme.accent
                        font.pixelSize: 10
                    }
                    Label {
                        // Hiding goes for layers; the camera and the world are always there.
                        visible: row.modelData.index >= 0
                        text: row.modelData.visible ? "●" : "○"
                        color: row.modelData.visible ? win.text : win.muted
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -4
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { backend.toggleVisible(row.modelData.index); outliner.done() }
                        }
                    }
                }
            }
        }
    }
}
