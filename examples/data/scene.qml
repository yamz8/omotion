import QtQuick
import Omotion

// A data story from the data template: how many of film 1's 450 frames
// omotion encodes again after a change, measured on 2026-10-23.
Rectangle {
    id: scene

    property real t: 0
    property real duration: 15
    property int fps: 30
    property string format: "16:9"
    property var formats: ["16:9", "9:16", "1:1", "4:5"]
    property var beats: [
        { at: 0, name: "Question", purpose: "what the numbers answer",
          enter: "the first word, already on screen", exit: "the question, read and held" },
        { at: 3, name: "Numbers", purpose: "the evidence, all of it",
          enter: "the question moves up", exit: "every bar at its value, labelled" },
        { at: 7, name: "Insight", purpose: "what the numbers show",
          enter: "every bar", exit: "the answer's bar in the accent, the rest dimmed, the insight above" },
        { at: 11, name: "Source", purpose: "why to believe it",
          enter: "the insight", exit: "the insight and the chart held, the source under them, as the poster" }
    ]
    property var cues: [{ at: 7, name: "answer", sound: "hit", on: "Insight" }]
    // The numbers, from the brief only.
    property var numbers: [
        { label: "A first render", value: 450 },
        { label: "A one-line patch", value: 96 },
        { label: "No change", value: 0 }
    ]
    property int answer: 1
    property string unit: ""

    readonly property bool wide: width > height * 1.2
    readonly property real u: Math.min(width, height) / 1080
    readonly property var clear: Motion.safe(format, width, height)
    readonly property real side: clear.x + (wide ? 60 : 40) * u
    readonly property real head: clear.y + (wide ? 50 * u : clear.height * 0.14)
    readonly property real across: width - 2 * side
    readonly property real headlineSize: Motion.per(format, { "1:1": 80 }, wide ? 88 : 96) * u
    readonly property real labelSize: Math.max(wide ? 50 : 30, (wide ? 50 : 40) * u)
    readonly property real highest: Math.max(...numbers.map(d => d.value))

    width: 1920
    height: 1080
    color: Style.colors.background

    FontLoader { source: "assets/AdwaitaSans-Regular.ttf" }
    FontLoader { source: "assets/LiberationMono-Regular.ttf" }

    component Headline: Flow {
        id: line
        property string text
        property real from
        property real to
        property bool first: false
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

    Headline { text: "How much does a patch render again?"; from: 0; to: 7; first: true }
    Headline { text: "Only the beat it changed."; from: 7.05; to: 16 }

    // The chart: a row per number, rising in turn.
    Column {
        id: chart
        x: scene.side
        // Under the headline: two lines in the wide frame, three in the tall ones.
        y: scene.head + (scene.wide ? 2 : 3) * (1.2 * scene.headlineSize + 22 * scene.u) + 50 * scene.u
        width: scene.across
        spacing: 0.7 * scene.labelSize
        opacity: Motion.progress(scene.t, 3, 3.3)
        visible: scene.t >= 3
        Repeater {
            model: scene.numbers
            delegate: Item {
                id: row
                required property var modelData
                required property int index
                readonly property real grow: Motion.rule("panel", scene.t, 3.3 + index * 0.25)
                readonly property bool lit: scene.t < 7 || index === scene.answer
                width: chart.width
                height: scene.labelSize * 1.3
                Text {
                    id: label
                    // Labels take more of a narrow frame.
                    width: chart.width * (scene.wide ? 0.3 : 0.45)
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.label
                    elide: Text.ElideRight
                    color: row.lit ? Style.colors.ink : Style.colors.dim
                    font.family: Style.fonts.headline
                    font.pixelSize: scene.labelSize
                }
                Rectangle {
                    id: bar
                    x: label.width + 20 * scene.u
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height * 0.7
                    width: Math.max(0, (chart.width * (scene.wide ? 0.55 : 0.36)) * row.modelData.value / scene.highest * Motion.clamp(row.grow, 0, 1.05))
                    radius: 6 * scene.u
                    color: scene.t >= 7 && row.index === scene.answer ? Style.colors.accent
                           : row.lit ? Style.colors.ink : Style.colors.line
                }
                Text {
                    x: bar.x + bar.width + 16 * scene.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.round(row.modelData.value * Motion.clamp(row.grow, 0, 1)) + scene.unit
                    color: row.lit ? Style.colors.ink : Style.colors.dim
                    font.family: Style.fonts.code
                    font.pixelSize: scene.labelSize
                }
            }
        }
    }

    Text {
        x: scene.side
        y: chart.y + chart.height + 50 * scene.u
        width: scene.across
        wrapMode: Text.WordWrap
        text: "Frames encoded for omotion's 15-second film 1, measured on 2026-10-23"
        color: Style.colors.dim
        font.family: Style.fonts.headline
        font.pixelSize: scene.labelSize
        opacity: Motion.progress(scene.t, 11.05, 11.35)
        visible: scene.t >= 11
    }
}
