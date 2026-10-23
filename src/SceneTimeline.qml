import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The scene's beats laid end to end, with the playhead; drag anywhere to scrub.
// Time runs left to right in every language, as it does in the render.
Item {
    id: timeline

    property real time: 0
    property real duration: 0
    property var beats: []
    property int current: -1
    property var notes: []
    property string selectedNote: ""

    signal seek(real time)
    signal noteClicked(string id)

    implicitHeight: 40
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    function xFor(t) {
        return duration > 0 ? Math.max(0, Math.min(1, t / duration)) * width : 0
    }

    Rectangle {
        id: track
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        color: "#1b1b20"
        clip: true

        Repeater {
            model: timeline.beats
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property real end: index + 1 < timeline.beats.length ? timeline.beats[index + 1].at : timeline.duration
                x: timeline.xFor(modelData.at)
                width: Math.max(0, timeline.xFor(end) - x - 2)
                height: track.height
                radius: Math.min(4, theme.radius)
                color: index === timeline.current ? Qt.alpha(theme.accent, 0.22) : index % 2 ? "#24242a" : "#202026"

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 6
                    verticalAlignment: Text.AlignVCenter
                    text: modelData.name
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    color: index === timeline.current ? "#f2f2f2" : "#9a9aa2"
                }
            }
        }
    }

    Rectangle {
        x: timeline.xFor(timeline.time) - width / 2
        width: 2
        height: parent.height + 8
        y: -4
        color: theme.accent
        visible: timeline.duration > 0
    }

    // The person's notes, above the beats, where they were pinned.
    Repeater {
        model: timeline.notes
        delegate: Rectangle {
            required property var modelData
            x: timeline.xFor(modelData.at) - width / 2
            y: -14
            width: modelData.id === timeline.selectedNote ? 10 : 8
            height: width
            radius: width / 2
            color: modelData.done ? "#4a4a52" : "#FF2D78"
            z: 2
            TapHandler { onTapped: timeline.noteClicked(parent.modelData.id) }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }

    MouseArea {
        anchors.fill: parent
        preventStealing: true
        cursorShape: Qt.PointingHandCursor
        enabled: timeline.duration > 0
        onPressed: mouse => timeline.seek(Math.max(0, Math.min(1, mouse.x / width)) * timeline.duration)
        onPositionChanged: mouse => {
            if (pressed)
                timeline.seek(Math.max(0, Math.min(1, mouse.x / width)) * timeline.duration)
        }
    }
}
