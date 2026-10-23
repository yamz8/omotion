import QtQuick
import Omotion

// A five-beat launch film: hook, problem, reveal, proof, and close, made
// only from the style guide's tokens, its motion rules, and states. Every
// word in [brackets] and every asset is the person's to supply: replace them
// from brief.md and assets/, and keep the structure.
Rectangle {
    id: scene

    property real t: 0
    property real duration: 15
    property int fps: 30
    property string format: "16:9"
    property var formats: ["16:9", "9:16", "1:1", "4:5"]
    property var beats: [
        { at: 0, name: "Hook", purpose: "the claim, before anyone scrolls past",
          enter: "the first word, already on screen", exit: "the claim, read and held" },
        { at: 3, name: "Problem", purpose: "what goes wrong without the product",
          enter: "the headline clears", exit: "the old way in a small card, and nothing else" },
        { at: 6, name: "Reveal", purpose: "the answer, as the thing itself",
          enter: "the card", exit: "the card grown into what the product does, the first thing in the accent" },
        { at: 9, name: "Proof", purpose: "it's real",
          enter: "the list clears", exit: "the real screen, the camera close on what proves the claim" },
        { at: 12, name: "Close", purpose: "what to do next",
          enter: "the screen clears", exit: "the logo, the name, and the call to action, held as the poster" }
    ]
    property var cues: [
        { at: 6, name: "reveal", sound: "hit", on: "Reveal" },
        { at: 12, name: "close", sound: "click", on: "Close" }
    ]

    // Laid out from the format, inside what the platform leaves clear.
    readonly property bool wide: width > height * 1.2
    readonly property real u: Math.min(width, height) / 1080
    readonly property var clear: Motion.safe(format, width, height)
    readonly property real side: clear.x + (wide ? 60 : 40) * u
    // The tall formats start lower, so the stack fills the height.
    readonly property real head: clear.y + (wide ? 50 * u : clear.height * 0.14)
    readonly property real across: width - 2 * side
    // Under the headline: one line in the wide frame, up to three in the tall ones.
    readonly property real below: head + (wide ? 190 * u : 3 * (1.2 * headlineSize + 22 * u) + 30 * u)
    // Square takes tighter type than the tall formats.
    readonly property real headlineSize: Motion.per(format, { "1:1": 88 }, wide ? 96 : 108) * u
    readonly property real captionSize: Math.max(wide ? 52 : 30, 52 * u)
    readonly property real codeSize: Math.max(wide ? 50 : 30, (wide ? 50 : 38) * u)

    // How much of something is on screen: in over a quarter second from
    // "from", out over a quarter second before "to".
    function shown(from, to) {
        return Motion.progress(t, from, from + 0.25) * (1 - Motion.progress(t, to - 0.25, to))
    }

    width: 1920
    height: 1080
    color: Style.colors.background

    FontLoader { source: "assets/AdwaitaSans-Regular.ttf" }
    FontLoader { source: "assets/LiberationMono-Regular.ttf" }

    // A headline that springs in word by word and holds.
    component Headline: Flow {
        id: line
        property string text
        property real from
        property real to
        property bool first: false // Already on screen at "from", for frame 0.
        x: scene.side
        y: scene.head
        width: scene.across
        spacing: 22 * scene.u
        opacity: 1 - Motion.progress(scene.t, to - 0.25, to)
        visible: scene.t >= from && scene.t < to
        Repeater {
            model: line.text.split(" ")
            delegate: Text {
                required property string modelData
                required property int index
                readonly property real arrive: line.first && index === 0 ? 1
                                               : Motion.rule("headline", scene.t, line.from + index * 0.07)
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

    Headline { text: "[The claim, in five words.]"; from: 0; to: 3; first: true }
    Headline { text: "[What goes wrong without it.]"; from: 3.05; to: 6 }
    Headline { text: "[The product] does it."; from: 6.05; to: 9 }

    // The card: one object from the old way to what the product does.
    Rectangle {
        id: card
        readonly property var state: Motion.between([
            { at: 3.3, x: scene.side, y: scene.below, w: scene.across * 0.7, h: 150 * scene.u },
            { at: 6.6, x: scene.side, y: scene.below, w: scene.across, h: 6 * 1.6 * scene.codeSize + 60 * scene.u,
              over: 0.5 }
        ], scene.t)
        x: state.x
        y: state.y
        width: state.w
        height: state.h
        radius: 18 * scene.u
        color: Style.colors.card
        border.color: Style.colors.line
        border.width: 2 * scene.u
        readonly property real arrive: Motion.rule("panel", scene.t, 3.3)
        opacity: Motion.clamp(arrive, 0, 1) * (1 - Motion.progress(scene.t, 8.75, 9))
        visible: scene.t >= 3.3 && scene.t < 9
        transform: Translate { y: (1 - card.arrive) * 30 * scene.u }

        Text {
            readonly property string full: "[The old way, in one line]"
            x: 36 * scene.u
            anchors.verticalCenter: parent.verticalCenter
            text: full.substring(0, Math.ceil(Motion.progress(scene.t, 3.6, 4.4) * full.length))
            color: Style.colors.ink
            font.family: Style.fonts.code
            font.pixelSize: scene.codeSize
            opacity: 1 - Motion.progress(scene.t, 6.0, 6.2)
            visible: opacity > 0
        }

        Column {
            x: 36 * scene.u
            y: 30 * scene.u
            spacing: 0.6 * scene.codeSize
            Repeater {
                model: ["1 [First thing it does]", "2 [Second thing]", "3 [Third thing]",
                        "4 [Fourth thing]", "5 [Fifth thing]", "6 [Sixth thing]"]
                delegate: Text {
                    required property string modelData
                    required property int index
                    readonly property real arrive: Motion.rule("micro", scene.t, 6.45 + index * 0.1)
                    readonly property bool yours: index === 0
                    text: modelData
                    color: yours ? Style.colors.accent : Style.colors.dim
                    font.family: Style.fonts.code
                    font.pixelSize: scene.codeSize
                    opacity: Motion.clamp(arrive, 0, 1)
                    transform: Translate { x: (1 - arrive) * 24 * scene.u }
                }
            }
        }
    }

    // Proof: the real screen, the camera moving in on what proves the claim.
    Text {
        id: proofCaption
        x: scene.side
        y: scene.head
        width: scene.across
        text: "[What the real screen proves.]"
        wrapMode: Text.WordWrap
        color: Style.colors.ink
        font.family: Style.fonts.headline
        font.pixelSize: scene.captionSize
        font.weight: Font.Bold
        opacity: scene.shown(9.05, 12)
        visible: opacity > 0
    }
    Item {
        id: window
        // The screenshot's own shape in the wide frame, as large as fits under
        // the caption; a tall box in the tall formats, the window cropped to
        // its middle until the camera moves to the note.
        readonly property real room: scene.clear.y + scene.clear.height - y - 20 * scene.u
        readonly property real shotHeight: scene.wide ? height : height
        readonly property real shotWidth: shotHeight * 1480 / 900
        x: scene.side
        y: proofCaption.y + proofCaption.height + 40 * scene.u
        width: scene.wide ? Math.min(scene.across, room * 1480 / 900) : scene.across
        height: scene.wide ? width * 900 / 1480 : Math.min(room, scene.across * 1.2)
        opacity: scene.shown(9.2, 12)
        visible: opacity > 0
        Camera {
            id: camera
            anchors.fill: parent
            t: scene.t
            // The camera ends on the part of the screen that proves the claim: here,
            // (1330, 170) of a 1480 x 900 screenshot. Set it from the real one.
            readonly property real shotX: (window.width - window.shotWidth) / 2
            readonly property real closeZoom: scene.wide ? 2.2 : 1.8
            // On the note, but never so far that the view leaves the picture.
            keys: [{ at: 9.6, zoom: 1 },
                   { at: 10.9, zoom: closeZoom, over: 1.2,
                     x: Math.min(shotX + window.shotWidth * 1330 / 1480,
                                 shotX + window.shotWidth - window.width / (2 * closeZoom)),
                     y: Math.max(window.height / (2 * closeZoom), window.shotHeight * 170 / 900) }]
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

    // Close: the logo, the name, and the call to action, held as the poster.
    Column {
        id: closing
        anchors.centerIn: parent
        spacing: 28 * scene.u
        readonly property real arrive: Motion.rule("panel", scene.t, 12.1)
        opacity: Motion.clamp(arrive, 0, 1)
        visible: scene.t >= 12
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
            text: "[Price or availability]"
            color: Style.colors.dim
            font.family: Style.fonts.headline
            font.pixelSize: scene.captionSize
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: command.width + 60 * scene.u
            height: command.height + 36 * scene.u
            radius: 14 * scene.u
            color: Style.colors.card
            border.color: Style.colors.accent
            border.width: 2 * scene.u
            Text {
                id: command
                anchors.centerIn: parent
                text: "[The call to action]"
                color: Style.colors.accent
                font.family: Style.fonts.code
                font.pixelSize: scene.codeSize
            }
        }
    }
}
