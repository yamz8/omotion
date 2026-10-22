import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// Every beat at once, each a live copy of the scene at the middle of its beat,
// so an edit shows up across the whole film. Clicking a tile goes there.
Item {
    id: sheet

    property var component: null
    property string format: "16:9"
    property var times: []
    property var beats: []
    property int fps: 30
    property int current: -1

    signal picked(real time)

    readonly property size frameSize: backend.formatSize(format)
    readonly property bool portrait: frameSize.height > frameSize.width

    function beatIndexAt(t) {
        let index = -1
        for (let i = 0; i < beats.length; ++i)
            if (beats[i].at <= t + 1e-9)
                index = i
        return index
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.margins: 16
        clip: true
        readonly property int columns: Math.max(1, Math.min(sheet.times.length,
                                                            Math.floor(width / (sheet.portrait ? 230 : 380))))
        cellWidth: Math.floor(width / columns)
        cellHeight: (cellWidth - 16) * sheet.frameSize.height / sheet.frameSize.width + 64
        model: sheet.times
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell
            required property real modelData
            required property int index
            readonly property int beat: sheet.beatIndexAt(modelData)
            width: grid.cellWidth
            height: grid.cellHeight

            Rectangle {
                id: tile
                x: 8
                y: 8
                width: parent.width - 16
                height: width * sheet.frameSize.height / sheet.frameSize.width
                color: "black"
                radius: Math.min(4, theme.radius)
                border.width: 2
                border.color: cell.beat === sheet.current && cell.beat >= 0 ? theme.accent
                              : hover.hovered ? "#55555d" : "#2c2c33"

                ScenePreview {
                    anchors.fill: parent
                    anchors.margins: 2
                    component: sheet.component
                    format: sheet.format
                    time: cell.modelData
                }

                HoverHandler {
                    id: hover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: sheet.picked(cell.modelData)
                }
            }

            Label {
                anchors.left: tile.left
                anchors.right: tile.right
                anchors.top: tile.bottom
                anchors.topMargin: 6
                text: (cell.index + 1).toLocaleString(Qt.locale(), 'f', 0) + "  "
                      + (cell.beat >= 0 ? sheet.beats[cell.beat].name : "")
                elide: Text.ElideRight
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: "#f2f2f2"
            }
            Label {
                anchors.left: tile.left
                anchors.right: tile.right
                anchors.top: tile.bottom
                anchors.topMargin: 28
                text: (translator.language,
                       qsTr("%1 s · frame %2").arg(cell.modelData.toLocaleString(Qt.locale(), 'f', 2))
                                              .arg(Math.round(cell.modelData * sheet.fps).toLocaleString(Qt.locale(), 'f', 0)))
                elide: Text.ElideRight
                font.pixelSize: 12
                color: "#9a9aa2"
            }
        }
    }
}
