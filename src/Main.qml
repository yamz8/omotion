import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 760
    minimumHeight: 520
    visible: true
    title: backend.sceneName ? backend.sceneName + " — omotion" : "omotion"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property real time: startTime
    property bool playing: false
    property string format: startFormat !== "" ? startFormat.replace("x", ":") : "16:9"
    property bool sheetVisible: startSheet
    property bool helpVisible: false
    property var sceneComponent: null
    property string loadError: ""

    readonly property var scene: preview.scene
    readonly property bool ready: scene !== null && preview.problem === "" && loadError === ""
    readonly property real duration: ready ? scene.duration : 0
    readonly property int fps: ready && scene.fps > 0 ? scene.fps : 30
    readonly property var beats: ready && scene.beats ? validBeats(scene.beats) : []
    readonly property var sceneFormats: ready && scene.formats && scene.formats.length
                                        ? Array.from(scene.formats).filter(f => backend.formatNames.indexOf(f) >= 0)
                                        : ["16:9"]
    readonly property real lastTime: duration > 0 ? Math.max(0, (Math.round(duration * fps) - 1) / fps) : 0
    readonly property int currentBeat: beatIndexAt(time)
    readonly property var sheetTimes: {
        if (duration <= 0)
            return []
        if (beats.length === 0)
            return [0, 1, 2, 3, 4, 5, 6, 7].map(i => duration * (i + 0.5) / 8)
        return beats.map((beat, i) => (beat.at + (i + 1 < beats.length ? beats[i + 1].at : duration)) / 2)
    }

    // The beats the scene declares, in order and within its length; omotion
    // check names the ones it leaves out.
    function validBeats(list) {
        const valid = []
        for (let i = 0; i < list.length; ++i) {
            const beat = list[i]
            if (beat && typeof beat.at === "number" && beat.name && beat.at >= 0 && beat.at < scene.duration
                    && (valid.length === 0 || beat.at > valid[valid.length - 1].at))
                valid.push({ at: beat.at, name: String(beat.name) })
        }
        return valid
    }

    function beatIndexAt(t) {
        let index = -1
        for (let i = 0; i < beats.length; ++i)
            if (beats[i].at <= t + 1e-9)
                index = i
        return index
    }

    function seek(t) {
        time = Math.max(0, Math.min(lastTime, t))
        if (playing)
            player.position = time * 1000
    }

    function step(frames) {
        playing = false
        seek((Math.round(time * fps) + frames) / fps)
    }

    function jumpBeat(direction) {
        if (beats.length === 0)
            return
        playing = false
        const index = beatIndexAt(time)
        if (direction < 0) {
            // Back to the start of this beat first, as a player's previous does.
            const atStart = index >= 0 && Math.abs(time - beats[index].at) < 0.5 / fps
            const target = atStart ? index - 1 : index
            seek(target >= 0 ? beats[target].at : 0)
        } else if (index + 1 < beats.length) {
            seek(beats[index + 1].at)
        }
    }

    function togglePlay() {
        if (!ready)
            return
        if (!playing && time >= lastTime)
            time = 0
        playing = !playing
    }

    function cycleFormat() {
        const index = sceneFormats.indexOf(format)
        format = sceneFormats[(index + 1) % sceneFormats.length]
    }

    function exportFormats(all) {
        if (ready && !backend.exporting)
            backend.exportVideo(all ? sceneFormats : [format])
    }

    function quit() {
        if (backend.exporting)
            quitDialog.open()
        else
            Qt.quit()
    }

    function loadScene() {
        sceneComponent = null
        reloadTimer.restart()
    }

    // The old copies go first, so the cache can let go of their files and the
    // new revision compiles everything beside the scene afresh.
    Timer {
        id: reloadTimer
        interval: 30
        onTriggered: {
            backend.trimCache()
            if (backend.scenePath === "") {
                win.loadError = ""
                return
            }
            const component = Qt.createComponent(backend.sceneUrl, Component.PreferSynchronous)
            if (component.status === Component.Error) {
                win.loadError = component.errorString()
                win.sceneComponent = null
            } else {
                win.loadError = ""
                win.sceneComponent = component
            }
        }
    }

    Connections {
        target: backend
        function onSceneChanged() { win.loadScene() }
    }

    Component.onCompleted: loadScene()

    onSceneChanged: {
        if (ready && sceneFormats.indexOf(format) < 0)
            format = sceneFormats[0]
        if (ready && time > lastTime)
            time = lastTime
    }

    onPlayingChanged: {
        if (playing && player.source != "") {
            player.position = time * 1000
            player.play()
        } else {
            player.pause()
        }
    }

    FrameAnimation {
        running: win.playing && win.ready
        onTriggered: {
            const next = win.time + frameTime
            if (next >= win.lastTime) {
                win.time = win.lastTime
                win.playing = false
            } else {
                win.time = next
            }
        }
    }

    MediaPlayer {
        id: player
        source: win.ready && win.scene.audio ? backend.resolve(win.scene.audio) : ""
        audioOutput: AudioOutput {}
    }

    // "?" is Shift+/ on most layouts, and arrives either way.
    Shortcut { sequences: ["?", "Shift+?"]; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: win.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: win.togglePlay() }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: win.step(-1) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: win.step(1) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; onActivated: win.step(-win.fps) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; onActivated: win.step(win.fps) }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; onActivated: win.jumpBeat(-1) }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; onActivated: win.jumpBeat(1) }
    Shortcut { sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: { win.playing = false; win.seek(0) } }
    Shortcut { sequence: "End"; context: Qt.ApplicationShortcut; onActivated: { win.playing = false; win.seek(win.lastTime) } }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; onActivated: win.cycleFormat() }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; onActivated: if (win.ready) win.sheetVisible = !win.sheetVisible }
    Shortcut { sequence: "E"; context: Qt.ApplicationShortcut; onActivated: win.exportFormats(false) }
    Shortcut { sequence: "Shift+E"; context: Qt.ApplicationShortcut; onActivated: win.exportFormats(true) }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; onActivated: backend.openScene() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; onActivated: backend.newScene() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; onActivated: backend.reload() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else
                win.sheetVisible = false
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction. The scene itself never mirrors.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Item {
                id: stage
                Layout.fillWidth: true
                Layout.fillHeight: true

                ScenePreview {
                    id: preview
                    anchors.fill: parent
                    anchors.margins: 24
                    visible: !win.sheetVisible && backend.scenePath !== ""
                    component: win.sceneComponent
                    format: win.format
                    time: win.time
                    outlined: true
                }

                ContactSheet {
                    anchors.fill: parent
                    visible: win.sheetVisible && win.ready
                    component: visible ? win.sceneComponent : null
                    format: win.format
                    times: win.sheetTimes
                    beats: win.beats
                    fps: win.fps
                    current: win.currentBeat
                    onPicked: t => {
                        win.seek(t)
                        win.sheetVisible = false
                    }
                }

                // Nothing open yet.
                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 520)
                    visible: backend.scenePath === ""
                    spacing: 14

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "omotion"
                        font.pixelSize: 40
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Preview motion scenes written as functions of time, look at every beat at once, and render each format.")
                        color: "#b4b4bb"
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 10
                        spacing: 12
                        Button {
                            Material.roundedScale: theme.radius
                            text: qsTr("Open Scene…")
                            highlighted: true
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.openScene()
                        }
                        Button {
                            Material.roundedScale: theme.radius
                            text: qsTr("New Scene…")
                            flat: true
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.newScene()
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 16
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Agents write scenes. Run %1 to teach yours how.").arg("omotion skill install")
                        color: "#7d7d85"
                        font.pixelSize: 13
                    }
                }

                // A scene that doesn't load says why, where its author is looking.
                Rectangle {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 860)
                    height: Math.min(parent.height - 48, errorColumn.implicitHeight + 40)
                    visible: backend.scenePath !== "" && (win.loadError !== "" || preview.problem !== "")
                    radius: theme.radius
                    color: "#1a1214"
                    border.color: "#6b2a32"

                    ColumnLayout {
                        id: errorColumn
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 10

                        Label {
                            text: qsTr("%1 didn’t load").arg(backend.sceneName)
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: "#ff8a95"
                        }
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredHeight: errorText.implicitHeight
                            TextArea {
                                id: errorText
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                text: win.loadError !== "" ? win.loadError : preview.problem
                                font.family: "monospace"
                                font.pixelSize: 13
                                background: null
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Fix the file and save it; omotion reloads by itself.")
                            color: "#b4b4bb"
                        }
                    }
                }

                // The last error the scene raised while running.
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 24
                    height: warningLabel.implicitHeight + 16
                    visible: backend.warning !== "" && win.loadError === ""
                    radius: Math.min(4, theme.radius)
                    color: "#2a2014"
                    border.color: "#6b5122"
                    Label {
                        id: warningLabel
                        anchors.fill: parent
                        anchors.margins: 8
                        text: backend.warning
                        elide: Text.ElideRight
                        font.family: "monospace"
                        font.pixelSize: 12
                        color: "#ffd38a"
                    }
                }

                // What an export did.
                Rectangle {
                    id: toast
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 24
                    width: Math.min(parent.width - 48, toastRow.implicitWidth + 28)
                    height: 48
                    visible: backend.message !== ""
                    radius: theme.radius
                    color: backend.messageIsError ? "#2a1418" : "#1d1d22"
                    border.color: backend.messageIsError ? "#6b2a32" : "#34343c"

                    RowLayout {
                        id: toastRow
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 8
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: backend.message
                            elide: Text.ElideMiddle
                            color: backend.messageIsError ? "#ff8a95" : "#e6e6ea"
                        }
                        Button {
                            Material.roundedScale: theme.radius
                            visible: backend.outputs.length > 0
                            text: qsTr("Show in Folder")
                            flat: true
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.showOutputs()
                        }
                        ToolButton {
                            text: "✕"
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.clearMessage()
                        }
                    }

                    Timer {
                        running: toast.visible && !backend.messageIsError
                        interval: 10000
                        onTriggered: backend.clearMessage()
                    }
                }
            }

            // The transport: play, where we are, the beats, format, sheet, export.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: "#141417"
                visible: backend.scenePath !== ""

                Rectangle {
                    anchors.top: parent.top
                    width: parent.width
                    height: 1
                    color: "#24242a"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 14

                    AbstractButton {
                        id: playButton
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 44
                        focusPolicy: Qt.NoFocus
                        enabled: win.ready
                        onClicked: win.togglePlay()
                        ToolTip.visible: hovered
                        ToolTip.text: win.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                        background: Rectangle {
                            radius: width / 2
                            color: !playButton.enabled ? "#2a2a30"
                                   : playButton.down ? Qt.darker(theme.accent, 1.25) : theme.accent
                        }
                        contentItem: Item {
                            // Drawn rather than typed, so it looks the same in every font.
                            Canvas {
                                anchors.centerIn: parent
                                width: 16
                                height: 16
                                property bool playing: win.playing
                                property color ink: theme.accentForeground
                                onPlayingChanged: requestPaint()
                                onInkChanged: requestPaint()
                                onPaint: {
                                    const g = getContext("2d")
                                    g.reset()
                                    g.fillStyle = ink
                                    if (playing) {
                                        g.fillRect(2, 1, 4, 14)
                                        g.fillRect(10, 1, 4, 14)
                                    } else {
                                        g.beginPath()
                                        g.moveTo(3, 1)
                                        g.lineTo(15, 8)
                                        g.lineTo(3, 15)
                                        g.closePath()
                                        g.fill()
                                    }
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        spacing: 0
                        Layout.preferredWidth: 150
                        Label {
                            text: (translator.language,
                                   qsTr("%1 / %2 s").arg(win.time.toLocaleString(Qt.locale(), 'f', 2))
                                                    .arg(win.duration.toLocaleString(Qt.locale(), 'f', 2)))
                            font.pixelSize: 15
                            font.features: { "tnum": 1 }
                        }
                        Label {
                            text: (translator.language,
                                   qsTr("frame %1").arg(Math.round(win.time * win.fps).toLocaleString(Qt.locale(), 'f', 0)))
                            color: "#8d8d95"
                            font.pixelSize: 12
                            font.features: { "tnum": 1 }
                        }
                    }

                    SceneTimeline {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        time: win.time
                        duration: win.duration
                        beats: win.beats
                        current: win.currentBeat
                        onSeek: t => {
                            win.playing = false
                            win.seek(t)
                        }
                    }

                    Button {

                        Material.roundedScale: theme.radius
                        text: win.format
                        flat: true
                        focusPolicy: Qt.NoFocus
                        enabled: win.ready && win.sceneFormats.length > 1
                        font.features: { "tnum": 1 }
                        onClicked: win.cycleFormat()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Next format (F)")
                    }

                    Button {

                        Material.roundedScale: theme.radius
                        text: qsTr("Contact Sheet")
                        flat: true
                        checkable: true
                        checked: win.sheetVisible
                        focusPolicy: Qt.NoFocus
                        enabled: win.ready
                        onClicked: win.sheetVisible = !win.sheetVisible
                    }

                    Button {

                        Material.roundedScale: theme.radius
                        id: exportButton
                        visible: !backend.exporting
                        text: qsTr("Export")
                        highlighted: true
                        focusPolicy: Qt.NoFocus
                        enabled: win.ready
                        onClicked: exportMenu.open()

                        Menu {
                            id: exportMenu
                            y: -height - 8
                            Material.roundedScale: theme.radius
                            MenuItem {
                                text: qsTr("This Format (%1)").arg(win.format)
                                onTriggered: win.exportFormats(false)
                            }
                            MenuItem {
                                text: qsTr("Every Format (%1)").arg(win.sceneFormats.join(", "))
                                enabled: win.sceneFormats.length > 1
                                onTriggered: win.exportFormats(true)
                            }
                        }
                    }

                    RowLayout {
                        visible: backend.exporting
                        spacing: 8
                        ColumnLayout {
                            spacing: 4
                            Label {
                                text: (translator.language,
                                       qsTr("Rendering %1 · %2%").arg(backend.exportFormat)
                                           .arg(Math.floor(backend.exportProgress * 100).toLocaleString(Qt.locale(), 'f', 0)))
                                font.pixelSize: 12
                                color: "#c9c9cf"
                            }
                            ProgressBar {
                                Layout.preferredWidth: 150
                                value: backend.exportProgress
                            }
                        }
                        Button {
                            Material.roundedScale: theme.radius
                            text: qsTr("Cancel")
                            flat: true
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.cancelExport()
                        }
                    }
                }
            }
        }
    }

    ShortcutHelp {
        anchors.fill: parent
        visible: win.helpVisible
        onClosed: win.helpVisible = false
    }

    Dialog {
        id: quitDialog
        anchors.centerIn: parent
        modal: true
        Material.roundedScale: theme.radius
        background: Rectangle {
            radius: theme.radius
            color: "#1d1d22"
            border.color: "#34343c"
        }
        title: qsTr("Stop the export?")
        header: Label {
            text: quitDialog.title
            padding: 24
            bottomPadding: 8
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }
        Label {
            text: qsTr("A video is still rendering. Quitting stops it and keeps nothing from this export.")
            wrapMode: Text.WordWrap
            width: 380
        }
        footer: DialogButtonBox {
            background: null
            Button {
                Material.roundedScale: theme.radius
                text: qsTr("Keep Rendering")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                Material.roundedScale: theme.radius
                text: qsTr("Stop and Quit")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
        onAccepted: {
            backend.cancelExport()
            Qt.quit()
        }
    }
}
