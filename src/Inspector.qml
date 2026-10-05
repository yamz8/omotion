import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The selected layer's properties at the playhead, and the scene's settings.
// A ◆ marks an animated property; clicking it sets a keyframe for just that
// property here.
Rectangle {
    id: inspector
    color: win.panel

    signal done()

    readonly property var v: backend.values
    readonly property bool hasLayer: backend.selected >= 0 || backend.cameraSelected
    readonly property bool isParticles: !!(v && v.particles)

    // Compact fields; Material's own are sized for touch.
    component SmallField: TextField {
        implicitHeight: 28
        topPadding: 4
        bottomPadding: 4
        leftPadding: 8
        rightPadding: 8
        font.pixelSize: 13
        selectByMouse: true
        color: win.text
        background: Rectangle {
            radius: 4
            color: "#1b1b1f"
            border.color: parent.activeFocus ? theme.accent : win.line
        }
    }

    component NumberField: SmallField {
        id: field
        property real value: 0
        property int decimals: 2
        signal committed(real value)

        function format(n) {
            const fixed = Number(n).toFixed(decimals);
            return fixed.indexOf(".") < 0 ? fixed : fixed.replace(/0+$/, "").replace(/\.$/, "");
        }

        onValueChanged: if (!activeFocus) text = format(value)
        onActiveFocusChanged: if (!activeFocus) text = format(value)
        Component.onCompleted: text = format(value)
        horizontalAlignment: TextInput.AlignRight
        validator: DoubleValidator {}
        onAccepted: {
            const n = Number(text);
            if (!isNaN(n)) committed(n);
            inspector.done();
        }
    }

    component PropertyRow: RowLayout {
        id: row
        property string name
        property string label
        property real factor: 1
        property int decimals: 2
        readonly property bool animated: (inspector.v.animated || []).indexOf(name) >= 0
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: row.animated ? "◆" : "◇"
            color: row.animated ? theme.accent : win.muted
            font.pixelSize: 14
            MouseArea {
                anchors.fill: parent
                anchors.margins: -4
                cursorShape: Qt.PointingHandCursor
                onClicked: backend.keyProperty(row.name)
            }
        }
        Label { text: row.label; color: win.text; Layout.preferredWidth: 90; elide: Text.ElideRight }
        NumberField {
            Layout.fillWidth: true
            decimals: row.decimals
            value: (inspector.v[row.name] || 0) * row.factor
            onCommitted: function (n) { backend.setProperty(row.name, n / row.factor) }
        }
    }

    component SceneRow: RowLayout {
        id: srow
        property string name
        property string label
        property int decimals: 2
        Layout.fillWidth: true
        spacing: 8
        Label { text: srow.label; color: win.text; Layout.preferredWidth: 90 }
        NumberField {
            Layout.fillWidth: true
            decimals: srow.decimals
            value: backend.sceneInfo[srow.name] || 0
            onCommitted: function (n) { backend.setSceneValue(srow.name, n) }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: body.implicitHeight + 32
        clip: true

        ColumnLayout {
            id: body
            x: 16
            y: 16
            width: inspector.width - 32
            spacing: 6

            Label {
                text: inspector.hasLayer ? (inspector.v.name || "") : "No layer selected"
                color: inspector.hasLayer ? win.text : win.muted
                font.pixelSize: 17
                font.bold: inspector.hasLayer
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                visible: inspector.hasLayer && !backend.cameraSelected && !inspector.isParticles
                text: (inspector.v.source || "") + (inspector.v.parent ? "\nmoves with " + inspector.v.parent : "")
                color: win.muted
                font.pixelSize: 12
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            Label {
                visible: !inspector.hasLayer
                text: "Click a layer on the canvas, or Tab through them."
                color: win.muted
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            // The camera, when it's selected.
            ColumnLayout {
                visible: backend.cameraSelected
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 4
                PropertyRow { name: "x"; label: "X" }
                PropertyRow { name: "y"; label: "Y" }
                PropertyRow { name: "zoom"; label: "Zoom %"; factor: 100; decimals: 1 }
                PropertyRow { name: "rotation"; label: "Rotation°" }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "X and Y are the point of the picture at the middle of the view. Layers follow the camera as much as their Follow camera says."
                }
            }

            // A particle layer's settings.
            ColumnLayout {
                visible: inspector.isParticles
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 4
                component ParticleRow: RowLayout {
                    id: prow
                    property string name
                    property string label
                    property int decimals: 0
                    Layout.fillWidth: true
                    spacing: 8
                    Item { width: 14; height: 1 }
                    Label { text: prow.label; color: win.text; Layout.preferredWidth: 90 }
                    NumberField {
                        Layout.fillWidth: true
                        decimals: prow.decimals
                        value: inspector.isParticles ? (inspector.v.particles[prow.name] || 0) : 0
                        onCommitted: function (n) { backend.setParticle(prow.name, n) }
                    }
                }
                Label {
                    text: "Particles: " + (inspector.isParticles ? inspector.v.particles.kind : "")
                    color: win.text
                    font.bold: true
                }
                ParticleRow { name: "count"; label: "Count" }
                ParticleRow { name: "size"; label: "Size px"; decimals: 1 }
                ParticleRow { name: "speed"; label: "Speed px/s" }
                ParticleRow { name: "direction"; label: "Direction°" }
                ParticleRow { name: "spread"; label: "Spread°" }
                ParticleRow { name: "sway"; label: "Sway px" }
                ParticleRow { name: "twinkle"; label: "Twinkle"; decimals: 2 }
                ParticleRow { name: "seed"; label: "Seed" }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { width: 14; height: 1 }
                    Label { text: "Colour"; color: win.text; Layout.preferredWidth: 90 }
                    Rectangle {
                        width: 18; height: 18; radius: 3
                        color: inspector.isParticles ? inspector.v.particles.color : "white"
                        border.color: win.line
                    }
                    SmallField {
                        Layout.fillWidth: true
                        property string value: inspector.isParticles ? inspector.v.particles.color : ""
                        onValueChanged: if (!activeFocus) text = value
                        onActiveFocusChanged: if (!activeFocus) text = value
                        Component.onCompleted: text = value
                        onAccepted: { backend.setParticle("color", text); inspector.done() }
                    }
                }
                PropertyRow { name: "opacity"; label: "Opacity %"; factor: 100; decimals: 0 }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "Direction 90° is down. They loop with the scene; another seed scatters them differently."
                }
            }

            ColumnLayout {
                visible: inspector.hasLayer && !backend.cameraSelected && !inspector.isParticles
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 4
                PropertyRow { name: "x"; label: "X" }
                PropertyRow { name: "y"; label: "Y" }
                PropertyRow { name: "scale"; label: "Scale"; decimals: 3 }
                PropertyRow { name: "scaleX"; label: "Stretch X"; decimals: 3 }
                PropertyRow { name: "scaleY"; label: "Stretch Y"; decimals: 3 }
                PropertyRow { name: "rotation"; label: "Rotation°" }
                PropertyRow { name: "opacity"; label: "Opacity %"; factor: 100; decimals: 0 }
            }

            Label {
                visible: inspector.hasLayer && !backend.cameraSelected
                Layout.topMargin: 10
                text: "Look"
                color: win.text
                font.bold: true
            }
            ColumnLayout {
                visible: inspector.hasLayer && !backend.cameraSelected
                Layout.fillWidth: true
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { width: 14; height: 1 }
                    Label { text: "Blend"; color: win.text; Layout.preferredWidth: 90 }
                    ComboBox {
                        id: blendBox
                        Layout.fillWidth: true
                        implicitHeight: 30
                        font.pixelSize: 13
                        model: backend.blends
                        currentIndex: Math.max(0, backend.blends.indexOf(inspector.v.blend || "normal"))
                        onActivated: function (index) { backend.setBlend(backend.blends[index]); inspector.done() }
                        background: Rectangle { radius: 4; color: "#1b1b1f"; border.color: blendBox.activeFocus ? theme.accent : win.line }
                    }
                }
                PropertyRow { visible: !inspector.isParticles; name: "brightness"; label: "Brightness %"; factor: 100; decimals: 0 }
                PropertyRow { visible: !inspector.isParticles; name: "contrast"; label: "Contrast %"; factor: 100; decimals: 0 }
                PropertyRow { visible: !inspector.isParticles; name: "saturation"; label: "Saturation %"; factor: 100; decimals: 0 }
                PropertyRow { name: "blur"; label: "Blur px"; decimals: 1 }
                PropertyRow { name: "glow"; label: "Glow px"; decimals: 1 }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 8
                    Label {
                        text: inspector.v.ownFollow ? "●" : "○"
                        color: inspector.v.ownFollow ? theme.accent : win.muted
                        font.pixelSize: 12
                        Layout.preferredWidth: 14
                        ToolTip.visible: followMouse.containsMouse
                        ToolTip.text: inspector.v.ownFollow ? "Its own; click to follow its parent again" : "Its parent's"
                        MouseArea {
                            id: followMouse
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (inspector.v.ownFollow) backend.setFollow(-1)
                        }
                    }
                    Label { text: "Follow camera %"; color: win.text; Layout.preferredWidth: 90; elide: Text.ElideRight }
                    NumberField {
                        Layout.fillWidth: true
                        decimals: 0
                        value: (inspector.v.follow === undefined ? 1 : inspector.v.follow) * 100
                        onCommitted: function (n) { backend.setFollow(n / 100) }
                    }
                }
            }

            Label {
                visible: inspector.hasLayer
                Layout.topMargin: 6
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: inspector.v.keyEase ? theme.accent : win.muted
                text: inspector.v.keyEase
                      ? "On a keyframe · eases " + (inspector.v.keyEase.indexOf("cubic") === 0 ? "on its own curve" : inspector.v.keyEase)
                        + " (E changes; drag the curve's handles below)"
                      : "K keyframes every property here"
            }

            // The easing out of the keyframe here, as a curve: drag its two
            // handles to shape it. Up past the top overshoots.
            Item {
                id: curve
                visible: inspector.hasLayer && !!inspector.v.keyEase && inspector.v.keyEase !== "hold"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(width * .66)
                Layout.topMargin: 4
                property var live: null
                readonly property var handles: live || inspector.v.keyCurve || [0, 0, 1, 1]
                readonly property real pad: 12
                // x 0..1 across; y shown from -0.25 to 1.25, for overshoot.
                function px(x) { return pad + x * (width - 2 * pad) }
                function py(y) { return height - pad - (y + .25) / 1.5 * (height - 2 * pad) }
                function curveX(X) { return Math.max(0, Math.min(1, (X - pad) / (width - 2 * pad))) }
                function curveY(Y) { return Math.max(-2, Math.min(3, (height - pad - Y) / (height - 2 * pad) * 1.5 - .25)) }
                onHandlesChanged: plot.requestPaint()
                onWidthChanged: plot.requestPaint()

                Rectangle { anchors.fill: parent; radius: 6; color: "#16161a"; border.color: win.line }
                Canvas {
                    id: plot
                    anchors.fill: parent
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        const h = curve.handles;
                        // The 0..1 box: start bottom left, end top right.
                        ctx.strokeStyle = "#2c2c33";
                        ctx.lineWidth = 1;
                        ctx.strokeRect(curve.px(0), curve.py(1), curve.px(1) - curve.px(0), curve.py(0) - curve.py(1));
                        ctx.strokeStyle = "#55555e";
                        ctx.beginPath();
                        ctx.moveTo(curve.px(0), curve.py(0)); ctx.lineTo(curve.px(h[0]), curve.py(h[1]));
                        ctx.moveTo(curve.px(1), curve.py(1)); ctx.lineTo(curve.px(h[2]), curve.py(h[3]));
                        ctx.stroke();
                        ctx.strokeStyle = String(theme.accent);
                        ctx.lineWidth = 2.5;
                        ctx.beginPath();
                        ctx.moveTo(curve.px(0), curve.py(0));
                        ctx.bezierCurveTo(curve.px(h[0]), curve.py(h[1]), curve.px(h[2]), curve.py(h[3]), curve.px(1), curve.py(1));
                        ctx.stroke();
                    }
                }
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        required property int index
                        width: 14; height: 14; radius: 7
                        x: curve.px(curve.handles[index * 2]) - 7
                        y: curve.py(curve.handles[index * 2 + 1]) - 7
                        color: handleMouse.pressed ? "white" : theme.accent
                        border.color: "#0e0e10"
                        border.width: 2
                        MouseArea {
                            id: handleMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.SizeAllCursor
                            onPressed: {
                                backend.beginGesture();
                                curve.live = curve.handles.slice();
                            }
                            onPositionChanged: function (mouse) {
                                const at = mapToItem(curve, mouse.x, mouse.y);
                                const next = curve.live.slice();
                                next[parent.index * 2] = curve.curveX(at.x);
                                next[parent.index * 2 + 1] = curve.curveY(at.y);
                                curve.live = next;
                                backend.setCurve(next[0], next[1], next[2], next[3]);
                            }
                            onReleased: {
                                backend.endGesture();
                                curve.live = null;
                                inspector.done();
                            }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.topMargin: 14; Layout.bottomMargin: 6; height: 1; color: win.line }

            Label { text: "Scene"; color: win.text; font.bold: true }
            SceneRow { name: "width"; label: "Width"; decimals: 0 }
            SceneRow { name: "height"; label: "Height"; decimals: 0 }
            SceneRow { name: "fps"; label: "FPS" }
            SceneRow { name: "duration"; label: "Seconds" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: "Background"; color: win.text; Layout.preferredWidth: 90 }
                Rectangle {
                    width: 18; height: 18; radius: 3
                    color: backend.sceneInfo.background || "black"
                    border.color: win.line
                }
                SmallField {
                    id: bgField
                    Layout.fillWidth: true
                    property string value: backend.sceneInfo.background || ""
                    onValueChanged: if (!activeFocus) text = value
                    onActiveFocusChanged: if (!activeFocus) text = value
                    Component.onCompleted: text = value
                    onAccepted: { backend.setSceneValue("background", text); inspector.done() }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.muted
                font.pixelSize: 12
                text: "#00000000 is transparent, for WebM and GIF."
            }

            Rectangle { Layout.fillWidth: true; Layout.topMargin: 10; Layout.bottomMargin: 4; height: 1; color: win.line }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Sound"; color: win.text; font.bold: true; Layout.fillWidth: true }
                ToolButton {
                    text: backend.audio.source ? "Replace" : "Add"
                    font.pixelSize: 12
                    implicitHeight: 28
                    onClicked: backend.importAudioDialog()
                }
                ToolButton {
                    visible: !!backend.audio.source
                    text: "Remove"
                    font.pixelSize: 12
                    implicitHeight: 28
                    onClicked: backend.removeAudio()
                }
            }
            Label {
                visible: !!backend.audio.source
                text: backend.audio.name || ""
                color: win.muted
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            RowLayout {
                visible: !!backend.audio.source
                Layout.fillWidth: true
                spacing: 8
                Label { text: "Starts at s"; color: win.text; Layout.preferredWidth: 90 }
                NumberField {
                    Layout.fillWidth: true
                    decimals: 2
                    value: backend.audio.offset || 0
                    onCommitted: function (n) { backend.setAudioValue("offset", n) }
                }
            }
            RowLayout {
                visible: !!backend.audio.source
                Layout.fillWidth: true
                spacing: 8
                Label { text: "Volume %"; color: win.text; Layout.preferredWidth: 90 }
                NumberField {
                    Layout.fillWidth: true
                    decimals: 0
                    value: (backend.audio.volume === undefined ? 1 : backend.audio.volume) * 100
                    onCommitted: function (n) { backend.setAudioValue("volume", n / 100) }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.muted
                font.pixelSize: 12
                text: backend.audio.source ? "Plays along, and goes into MP4 and WebM exports." : "Ctrl+M adds music or sound to time things to."
            }
        }
    }
}
