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
    property var cues: []
    property var waveform: [] // The track's loudness over the film, 0 to 1.
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

    // The track along the bottom of the beats, so a cut can be seen against it.
    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 12
        visible: timeline.waveform.length > 0
        Repeater {
            model: timeline.waveform
            delegate: Rectangle {
                required property real modelData
                width: timeline.width / timeline.waveform.length
                height: Math.max(1, modelData * 12)
                anchors.bottom: parent.bottom
                color: "#5a5a64"
            }
        }
    }

    // Cues as ticks along the top, named on hover.
    Repeater {
        model: timeline.cues
        delegate: Rectangle {
            required property var modelData
            objectName: "cue"
            x: timeline.xFor(modelData.at) - width / 2
            y: 0
            width: 3
            height: 12
            color: "#7fd1ff"
            z: 1
            HoverHandler { id: cueHover }
            ToolTip.visible: cueHover.hovered
            ToolTip.text: modelData.sound ? modelData.name + " · " + modelData.sound : modelData.name
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
