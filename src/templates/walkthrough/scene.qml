import QtQuick
import Omotion

// A UI walkthrough in five beats: what the viewer will learn, three steps
// on the real screen, and the close. The camera moves to each step in
// "stops", given as fractions of the screenshot (0 to 1), which the agent
// sets from the real one. Every word in [brackets] and every asset is the
// person's to supply.
Rectangle {
    id: scene

    property real t: 0
    property real duration: 15
    property int fps: 30
    property string format: "16:9"
    property var formats: ["16:9", "9:16", "1:1", "4:5"]
    property var beats: [
        { at: 0, name: "Title", purpose: "what the viewer will be able to do",
          enter: "the first word, already on screen", exit: "the task, read and held" },
        { at: 2.5, name: "Find", purpose: "where to start",
          enter: "the title clears", exit: "the real screen, the camera on the first step" },
        { at: 5.5, name: "Choose", purpose: "the choice that matters",
          enter: "the first step", exit: "the camera on the second step" },
        { at: 8.5, name: "Result", purpose: "what the viewer gets",
          enter: "the second step", exit: "the camera on the result" },
        { at: 11.5, name: "Close", purpose: "what to do next",
          enter: "the screen clears", exit: "the logo, the name, and the call to action, held as the poster" }
    ]
    property var cues: [
        { at: 2.5, name: "find", sound: "tick", on: "Find" },
        { at: 5.5, name: "choose", sound: "tick", on: "Choose" },
        { at: 8.5, name: "result", sound: "click", on: "Result" }
    ]
    // Where each step is on the screenshot, as fractions of it, and how close.
    property var stops: [
        { at: 4.0, x: 0.2, y: 0.25, zoom: 1.8, caption: "1  [Where to start]" },
        { at: 7.0, x: 0.55, y: 0.45, zoom: 2.0, caption: "2  [What to choose]" },
        { at: 10.0, x: 0.8, y: 0.7, zoom: 1.8, caption: "3  [What you get]" }
    ]

    readonly property bool wide: width > height * 1.2
    readonly property real u: Math.min(width, height) / 1080
    readonly property var clear: Motion.safe(format, width, height)
    readonly property real side: clear.x + (wide ? 60 : 40) * u
    readonly property real head: clear.y + (wide ? 50 * u : clear.height * 0.14)
    readonly property real across: width - 2 * side
    readonly property real headlineSize: Motion.per(format, { "1:1": 88 }, wide ? 96 : 108) * u
    readonly property real captionSize: Math.max(wide ? 52 : 30, 52 * u)
    readonly property real codeSize: Math.max(wide ? 50 : 30, (wide ? 50 : 38) * u)

    function shown(from, to) {
        return Motion.progress(t, from, from + 0.25) * (1 - Motion.progress(t, to - 0.25, to))
    }

    width: 1920
    height: 1080
    color: Style.colors.background

    FontLoader { source: "assets/AdwaitaSans-Regular.ttf" }
    FontLoader { source: "assets/LiberationMono-Regular.ttf" }

    // Title: the task, word by word.
    Flow {
        x: scene.side
        y: scene.head
        width: scene.across
        spacing: 22 * scene.u
        opacity: 1 - Motion.progress(scene.t, 2.25, 2.5)
        visible: scene.t < 2.5
        Repeater {
            model: "[What the viewer will learn to do.]".split(" ")
            delegate: Text {
                required property string modelData
                required property int index
                readonly property real arrive: index === 0 ? 1 : Motion.rule("headline", scene.t, index * 0.07)
                text: modelData
                color: Style.colors.ink
                font.family: Style.fonts.headline
                font.pixelSize: scene.headlineSize
                font.weight: Font.Bold
                opacity: Motion.clamp(arrive, 0, 1)
                transform: Translate { y: (1 - arrive) * 40 * scene.u }
            }
        }
    }

    // Each step's caption above the screen, in place of the last.
    Repeater {
        model: scene.stops
        delegate: Text {
            required property var modelData
            required property int index
            readonly property real from: scene.beats[index + 1].at + 0.05
            readonly property real to: scene.beats[index + 2].at
            x: scene.side
            y: scene.head
            width: scene.across
            text: modelData.caption
            wrapMode: Text.WordWrap
            color: Style.colors.ink
            font.family: Style.fonts.headline
            font.pixelSize: scene.captionSize
            font.weight: Font.Bold
            opacity: scene.shown(from, to)
            visible: opacity > 0
        }
    }

    Item {
        id: window
        readonly property real room: scene.clear.y + scene.clear.height - y - 20 * scene.u
        readonly property real shotHeight: height
        readonly property real shotWidth: shotHeight * 16 / 10
        x: scene.side
        y: scene.head + 2.6 * scene.captionSize
        width: scene.wide ? Math.min(scene.across, room * 16 / 10) : scene.across
        height: scene.wide ? width * 10 / 16 : Math.min(room, scene.across * 1.2)
        opacity: scene.shown(2.6, 11.5)
        visible: opacity > 0
        Camera {
            id: camera
            anchors.fill: parent
            t: scene.t
            readonly property real shotX: (window.width - window.shotWidth) / 2
            keys: [{ at: 2.6, zoom: 1 }].concat(scene.stops.map(s => ({
                at: s.at, zoom: s.zoom, over: 1.2,
                x: Math.max(window.width / (2 * s.zoom),
                            Math.min(shotX + window.shotWidth * s.x, shotX + window.shotWidth - window.width / (2 * s.zoom))),
                y: Math.max(window.height / (2 * s.zoom),
                            Math.min(window.shotHeight * s.y, window.shotHeight - window.height / (2 * s.zoom)))
            })))
            Image {
                id: shot
                x: camera.shotX
                width: window.shotWidth
                height: window.shotHeight
                source: "assets/screens/product.png"
                smooth: true
                mipmap: true
            }
        }
        // Until the real screen is in assets/, the frame says where it goes;
        // outside the camera, so it holds still.
        Text {
            anchors.centerIn: parent
            visible: shot.status !== Image.Ready
            text: "assets/screens/product.png"
            color: Style.colors.dim
            font.family: Style.fonts.code
            font.pixelSize: scene.codeSize
        }
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: Style.colors.line
            border.width: 2 * scene.u
        }
    }

    Column {
        id: closing
        anchors.centerIn: parent
        spacing: 28 * scene.u
        readonly property real arrive: Motion.rule("panel", scene.t, 11.6)
        opacity: Motion.clamp(arrive, 0, 1)
        visible: scene.t >= 11.5
        transform: Translate { y: (1 - closing.arrive) * 40 * scene.u }
        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            source: "assets/logo.svg"
            width: 160 * scene.u
            height: width
            sourceSize: Qt.size(width, height)
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "[Product]"
            color: Style.colors.ink
            font.family: Style.fonts.headline
            font.pixelSize: scene.headlineSize * 1.2
            font.weight: Font.Bold
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "[The call to action]"
            color: Style.colors.accent
            font.family: Style.fonts.code
            font.pixelSize: scene.codeSize
        }
    }
}
