import QtQuick
import Omotion

// An omotion scene: everything on screen is computed from t, so any frame can
// be rendered on its own, in any order. Run "omotion help format" for the rules.
Rectangle {
    id: scene

    property real t: 0
    property real duration: 9
    property int fps: 30
    property string format: "16:9"
    property var formats: ["16:9", "9:16"]
    // Each beat matches one in shotlist.md: why it exists, and the states it
    // starts from and must reach, copied from the approved shot list.
    property var beats: [
        { at: 0.0, name: "Hook", purpose: "say what the film is about before anyone scrolls past",
          enter: "the first word, already on screen", exit: "the headline, read and gone" },
        { at: 2.2, name: "Frames", purpose: "show that a frame is a moment, not a recording",
          enter: "an empty frame", exit: "five moments of one dot, side by side" },
        { at: 4.8, name: "Review", purpose: "show the frames being checked before release",
          enter: "five moments of one dot", exit: "each moment passed over" },
        { at: 7.2, name: "Ship", purpose: "leave the viewer with the idea and the name",
          enter: "the frames clear", exit: "the closing line and the mark, held as the poster" }
    ]

    // Type and spacing follow the short side, and the layout follows the
    // format, so the vertical cut is composed rather than cropped.
    readonly property bool portrait: height > width
    readonly property real unit: Math.min(width, height) / 1080
    readonly property real margin: (portrait ? 90 : 140) * unit
    readonly property color ink: "#f4f1ea"
    readonly property color dim: "#8d8a84"
    readonly property color accent: "#ffd60a"

    width: 1920
    height: 1080
    color: "#0d0d10"

    // Hook: the headline arrives word by word, holds long enough to read, and leaves.
    Flow {
        id: headline
        x: scene.margin
        width: scene.width - 2 * scene.margin
        anchors.verticalCenter: parent.verticalCenter
        spacing: 30 * scene.unit
        readonly property real leave: Motion.easeIn(Motion.progress(scene.t, 1.9, 2.3))
        opacity: 1 - leave
        visible: opacity > 0

        Repeater {
            model: ["A", "scene", "is", "a", "function", "of", "time."]
            delegate: Text {
                required property string modelData
                required property int index
                // The first word is already there at frame 0, which is often the thumbnail.
                readonly property real arrive: index === 0 ? 1 : Motion.spring(scene.t, 0.06 + index * 0.09, 210, 22)
                text: modelData
                color: modelData === "time." ? scene.accent : scene.ink
                font.pixelSize: (scene.portrait ? 128 : 116) * scene.unit
                font.weight: Font.Bold
                font.letterSpacing: -2 * scene.unit
                opacity: Motion.clamp(arrive, 0, 1)
                transform: Translate { y: (1 - arrive) * 60 * scene.unit - headline.leave * 40 * scene.unit }
            }
        }
    }

    // A caption above the frames, which changes with the beat.
    Repeater {
        model: [
            { text: "Each frame shows what's on screen at t.", from: 2.4, to: 4.75 },
            { text: "Then check the frames before anyone watches.", from: 4.85, to: 7.2 }
        ]
        delegate: Text {
            required property var modelData
            readonly property real shown: Motion.easeOut(Motion.progress(scene.t, modelData.from, modelData.from + 0.25))
                                          * (1 - Motion.progress(scene.t, modelData.to - 0.25, modelData.to))
            x: scene.margin
            y: frames.y - height - 70 * scene.unit + (1 - shown) * 20 * scene.unit
            width: scene.width - 2 * scene.margin
            text: modelData.text
            color: scene.ink
            wrapMode: Text.WordWrap
            font.pixelSize: (scene.portrait ? 56 : 52) * scene.unit
            font.weight: Font.Medium
            opacity: shown
            visible: shown > 0
        }
    }

    // Frames, then Review: the same dot in five moments, then a pass over each.
    Grid {
        id: frames
        readonly property real tileWidth: (scene.portrait ? 440 : 300) * scene.unit
        readonly property real tileHeight: tileWidth * (scene.portrait ? 9 / 16 : 3 / 4)
        columns: scene.portrait ? 2 : 5
        spacing: (scene.portrait ? 34 : 40) * scene.unit
        anchors.horizontalCenter: parent.horizontalCenter
        y: (scene.height - height) / 2 + (scene.portrait ? 60 : 70) * scene.unit
        visible: scene.t > 2.2 && scene.t < 8.4

        Repeater {
            model: 5
            delegate: Rectangle {
                id: tile
                required property int index
                readonly property real arrive: Motion.spring(scene.t, 2.35 + index * 0.12, 260, 24)
                readonly property real leave: Motion.easeIn(Motion.progress(scene.t, 7.2 + index * 0.06, 7.75 + index * 0.06))
                // The review pass lights one frame at a time, then approves them all.
                readonly property int reviewing: Math.floor((scene.t - 5.2) / 0.32)
                readonly property real lit: scene.t >= 6.9 ? Motion.easeOut(Motion.progress(scene.t, 6.9, 7.15))
                                                           : reviewing === index ? 1 : 0

                width: frames.tileWidth
                height: frames.tileHeight
                radius: 18 * scene.unit
                color: "#17171c"
                border.color: lit > 0 ? scene.accent : "#2c2c33"
                border.width: (lit > 0 ? 4 : 2) * scene.unit
                opacity: Motion.clamp(arrive, 0, 1) * (1 - leave)
                       * (scene.t > 5.1 && scene.t < 6.9 && lit === 0 ? 0.45 : 1)
                transform: [
                    Scale {
                        origin.x: tile.width / 2
                        origin.y: tile.height / 2
                        xScale: 0.8 + 0.2 * tile.arrive
                        yScale: xScale
                    },
                    Translate { y: tile.leave * 160 * scene.unit }
                ]

                // The object every frame shares, where a spring has it at this frame's time.
                Rectangle {
                    readonly property real travel: Motion.spring(tile.index * 0.25, 0, 60, 6)
                    width: 34 * scene.unit
                    height: width
                    radius: width / 2
                    color: scene.accent
                    x: Motion.mix(24 * scene.unit, tile.width - width - 48 * scene.unit, travel / 1.25)
                    // In the space above the label.
                    y: (tile.height - 72 * scene.unit - height) / 2
                }

                Text {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 16 * scene.unit
                    text: "t = " + (tile.index * 0.25).toFixed(2)
                    color: scene.dim
                    // Readable in a feed: 10 px when the film plays 390 px wide.
                    font.pixelSize: (scene.portrait ? 34 : 50) * scene.unit
                    font.family: "monospace"
                }
            }
        }
    }

    // Ship: the closing line, and the mark.
    Column {
        id: closing
        anchors.centerIn: parent
        width: scene.width - 2 * scene.margin
        spacing: 36 * scene.unit
        readonly property real arrive: Motion.spring(scene.t, 7.55, 150, 20)
        opacity: Motion.clamp(arrive, 0, 1)
        visible: scene.t > 7.5
        transform: Translate { y: (1 - closing.arrive) * 50 * scene.unit }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Render the next one better."
            color: scene.ink
            font.pixelSize: (scene.portrait ? 112 : 120) * scene.unit
            font.weight: Font.Bold
            font.letterSpacing: -2 * scene.unit
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 14 * scene.unit
            opacity: Motion.progress(scene.t, 8.0, 8.4)

            Rectangle {
                width: 22 * scene.unit
                height: width
                radius: width / 2
                color: scene.accent
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: "omotion"
                color: scene.dim
                font.pixelSize: 52 * scene.unit
                font.weight: Font.Medium
            }
        }
    }
}
