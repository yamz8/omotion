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

    // A property, and under it the link that drives it when it has one: ⛓
    // opens the link for editing.
    component PropertyRow: ColumnLayout {
        id: row
        property string name
        property string label
        property real factor: 1
        property int decimals: 2
        readonly property bool animated: (inspector.v.animated || []).indexOf(name) >= 0
        readonly property string link: (inspector.v.links || {})[name] || ""
        property bool editingLink: false
        Layout.fillWidth: true
        spacing: 2

        RowLayout {
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
            Label {
                text: "⛓"
                font.family: "Noto Sans Symbols 2"
                font.pixelSize: 13
                color: row.link ? theme.accent : win.muted
                opacity: row.link || linkMouse.containsMouse ? 1 : .45
                ToolTip.visible: linkMouse.containsMouse
                ToolTip.text: row.link ? "Linked to " + row.link : "Link to another property"
                MouseArea {
                    id: linkMouse
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        row.editingLink = !row.editingLink;
                        if (row.editingLink) linkField.forceActiveFocus();
                    }
                }
            }
        }
        SmallField {
            id: linkField
            visible: row.editingLink || row.link !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 22
            font.pixelSize: 12
            color: theme.accent
            // Only while empty: Material floats a placeholder above the text.
            placeholderText: text.length ? "" : "car.x × 0.8 · speed of car.x · time × 90"
            onActiveFocusChanged: if (!activeFocus) { text = row.link; row.editingLink = false }
            Component.onCompleted: text = row.link
            Connections {
                target: row
                function onLinkChanged() { if (!linkField.activeFocus) linkField.text = row.link }
            }
            Keys.onEscapePressed: inspector.done()
            onAccepted: {
                if (backend.setLink(row.name, text)) row.editingLink = false;
                inspector.done();
            }
        }
    }

    component WorldNumber: RowLayout {
        id: wrow
        property string name
        property string label
        property real value
        property int decimals: 2
        Layout.fillWidth: true
        spacing: 8
        Label { text: wrow.label; color: win.text; Layout.preferredWidth: 90 }
        NumberField {
            Layout.fillWidth: true
            decimals: wrow.decimals
            value: wrow.value
            onCommitted: function (n) { backend.setWorldValue(wrow.name, n) }
        }
    }

    component WorldColour: RowLayout {
        id: crow
        property string name
        property string label
        property string value
        Layout.fillWidth: true
        spacing: 8
        Label { text: crow.label; color: win.text; Layout.preferredWidth: 90 }
        Rectangle { width: 18; height: 18; radius: 3; color: crow.value || "transparent"; border.color: win.line }
        SmallField {
            id: colourField
            Layout.fillWidth: true
            onActiveFocusChanged: if (!activeFocus) text = crow.value
            Component.onCompleted: text = crow.value
            Connections { target: crow; function onValueChanged() { if (!colourField.activeFocus) colourField.text = crow.value } }
            onAccepted: { backend.setWorldValue(crow.name, text); inspector.done() }
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
                text: inspector.hasLayer ? (inspector.v.name || "") : backend.world.active ? "World" : "No layer selected"
                color: inspector.hasLayer || backend.world.active ? win.text : win.muted
                font.pixelSize: 17
                font.bold: inspector.hasLayer || backend.world.active
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
            // How the layer came to be, step by step, each one can be done
            // again; what's out of date says why.
            ColumnLayout {
                visible: backend.selected >= 0 && backend.steps.length > 0
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 2
                Label { text: "Steps"; color: win.text; font.bold: true }
                Repeater {
                    model: backend.steps
                    delegate: ColumnLayout {
                        id: stepRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                text: stepRow.modelData.stale ? "⚠" : stepRow.modelData.done ? "✓" : "·"
                                color: stepRow.modelData.stale ? "#ffb35c" : stepRow.modelData.done ? theme.accent : win.muted
                                Layout.preferredWidth: 12
                            }
                            Label {
                                text: stepRow.modelData.title
                                color: stepRow.modelData.done ? win.text : win.muted
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 18
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            color: stepRow.modelData.stale ? "#ffb35c" : win.muted
                            text: stepRow.modelData.stale ? "Out of date: " + stepRow.modelData.why : stepRow.modelData.detail
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            spacing: 0
                            Repeater {
                                model: stepRow.modelData.actions
                                delegate: ToolButton {
                                    required property string modelData
                                    font.pixelSize: 12
                                    implicitHeight: 28
                                    text: ({"reshape": "Reshape", "ai": "Fill by AI", "smooth": "Fill smoothly", "none": "Leave open",
                                            "again": "Do again", "make": "Make"})[modelData] || modelData
                                    onClicked: { backend.runStep(stepRow.modelData.name, modelData); inspector.done() }
                                }
                            }
                        }
                    }
                }
            }

            // The rig: what drives it, as Blender's constraints do.
            ColumnLayout {
                visible: backend.selected >= 0 && !inspector.isParticles
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "Rig"; color: win.text; font.bold: true; Layout.fillWidth: true }
                    ToolButton {
                        visible: (inspector.v.rig || []).length > 0
                        text: "Remove"
                        font.pixelSize: 12
                        implicitHeight: 28
                        onClicked: { backend.unrigSelected(); inspector.done() }
                    }
                }
                Repeater {
                    model: inspector.v.rig || []
                    delegate: Label {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "⛓ " + modelData
                        color: theme.accent
                        font.pixelSize: 12
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    ComboBox {
                        id: rigType
                        Layout.preferredWidth: 110
                        font.pixelSize: 12
                        implicitHeight: 34
                        model: ["Reach with", "Look at", "Roll on"]
                    }
                    ComboBox {
                        id: rigOther
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        implicitHeight: 34
                        // A reach is with a child; the rest with any other layer.
                        model: {
                            const names = [];
                            const all = backend.layers;
                            const me = inspector.v.name;
                            for (let i = all.length - 1; i >= 0; --i)
                                if (all[i].name !== me && (rigType.currentIndex !== 0 || all[i].parent === me))
                                    names.push(all[i].name);
                            return names;
                        }
                    }
                    ToolButton {
                        text: "Rig"
                        enabled: rigOther.count > 0
                        font.pixelSize: 12
                        implicitHeight: 34
                        onClicked: { backend.rigSelected(["ik", "lookAt", "wheel"][rigType.currentIndex], rigOther.currentText); inspector.done() }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "Reach: an arm bending to reach for an empty (move the empty).  Shift+P draws a path to follow, Shift+A adds an empty."
                }
            }

            // A video layer: when it starts, how fast, looping or not.
            ColumnLayout {
                visible: backend.selected >= 0 && !!(inspector.v.video || {}).frames
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                readonly property var video: inspector.v.video || {}
                Label { text: "Video, " + (parent.video.count || 0) + " frames"; color: win.text; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Starts at s"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.video.start || 0; onCommitted: function (n) { backend.setVideo("start", n) } }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Speed ×"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.video.speed === undefined ? 1 : parent.parent.video.speed; onCommitted: function (n) { backend.setVideo("speed", n) } }
                }
                CheckBox {
                    text: "Loop"
                    checked: parent.video.loop !== false
                    onClicked: { backend.setVideo("loop", checked); inspector.done() }
                }
            }

            // Billowing, as smoke, cloud or water: a slow warp.
            ColumnLayout {
                visible: backend.selected >= 0 && !inspector.isParticles && !!inspector.v.source
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                readonly property var warp: inspector.v.warp || {}
                Label { text: "Billow"; color: win.text; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Amount px"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.warp.amount || 0; onCommitted: function (n) { backend.setWarp("amount", n) } }
                }
                RowLayout {
                    visible: (parent.warp.amount || 0) > 0
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Size px"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.warp.scale || 80; onCommitted: function (n) { backend.setWarp("scale", n) } }
                }
                RowLayout {
                    visible: (parent.warp.amount || 0) > 0
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Speed"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.warp.speed === undefined ? .3 : parent.parent.warp.speed; onCommitted: function (n) { backend.setWarp("speed", n) } }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "Smoke, cloud, dust or water moving: its picture pushed about by slow noise, looping with the scene. Cut the part that moves out first."
                }
            }

            // Cloth: the picture as fabric, pinned, in the wind.
            ColumnLayout {
                visible: backend.selected >= 0 && !inspector.isParticles && !!inspector.v.source
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                readonly property var cloth: inspector.v.cloth || {}
                readonly property bool on: cloth.pin !== undefined
                CheckBox {
                    text: "Cloth"
                    font.bold: true
                    checked: parent.on
                    onClicked: { backend.setCloth("on", checked); inspector.done() }
                    ToolTip.visible: hovered
                    ToolTip.text: "The picture as fabric: held where it's pinned, blown by the wind, folding"
                }
                RowLayout {
                    visible: parent.on
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Pinned"; color: win.text; Layout.preferredWidth: 90 }
                    ComboBox {
                        Layout.fillWidth: true
                        implicitHeight: 32
                        font.pixelSize: 12
                        model: ["left", "right", "top", "bottom", "top corners", "corners"]
                        currentIndex: Math.max(0, model.indexOf(parent.parent.cloth.pin || "left"))
                        onActivated: function (i) { backend.setCloth("pin", model[i]); inspector.done() }
                    }
                }
                Repeater {
                    model: [["wind", "Wind px/s", 300], ["direction", "Wind way°", 0], ["gust", "Gusts", .5],
                            ["stiffness", "Stiffness", .8], ["weight", "Weight", .3]]
                    delegate: RowLayout {
                        required property var modelData
                        visible: parent.on
                        Layout.fillWidth: true
                        spacing: 8
                        Label { text: modelData[1]; color: win.text; Layout.preferredWidth: 90 }
                        NumberField {
                            Layout.fillWidth: true
                            value: parent.parent.cloth[parent.modelData[0]] === undefined ? parent.modelData[2] : parent.parent.cloth[parent.modelData[0]]
                            onCommitted: function (n) { backend.setCloth(parent.modelData[0], n) }
                        }
                    }
                }
            }

            // An imported model's size.
            RowLayout {
                visible: backend.selected >= 0 && !!(inspector.v.model || {}).file
                Layout.fillWidth: true
                spacing: 8
                Label { text: "Size m"; color: win.text; Layout.preferredWidth: 90 }
                NumberField { Layout.fillWidth: true; value: (inspector.v.model || {}).size || 1; onCommitted: function (n) { backend.setModelSize(n) } }
            }

            // A light: its colour, power and reach.
            ColumnLayout {
                visible: backend.selected >= 0 && !!(inspector.v.light || {}).color
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                readonly property var light: inspector.v.light || {}
                Label { text: "Light"; color: win.text; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Colour"; color: win.text; Layout.preferredWidth: 90 }
                    Rectangle { width: 18; height: 18; radius: 3; color: parent.parent.light.color || "white"; border.color: win.line }
                    SmallField {
                        id: lightColour
                        Layout.fillWidth: true
                        readonly property string value: parent.parent.light.color || ""
                        onValueChanged: if (!activeFocus) text = value
                        Component.onCompleted: text = value
                        onAccepted: { backend.setLightValue("color", text); inspector.done() }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Power"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.light.power || 0; onCommitted: function (n) { backend.setLightValue("power", n) } }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Reach m"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.light.reach || 0; onCommitted: function (n) { backend.setLightValue("reach", n) } }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "It adds to the picture's own light, on what faces it, half as bright at its reach. Its opacity dims it, and keys like any."
                }
            }
            // A piece's material, under lights.
            ColumnLayout {
                visible: backend.selected >= 0 && !!inspector.v.inWorld && !(inspector.v.light || {}).color
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                readonly property var material: inspector.v.material || {}
                Label { text: "Material"; color: win.text; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Shine"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.material.shine || 0; onCommitted: function (n) { backend.setMaterial("shine", n) } }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Roughness"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; value: parent.parent.material.roughness === undefined ? .5 : parent.parent.material.roughness; onCommitted: function (n) { backend.setMaterial("roughness", n) } }
                }
            }

            // A 3D shape's mesh modifiers.
            ColumnLayout {
                visible: backend.selected >= 0 && !!inspector.v.shaped
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                function amount(type, key) {
                    const list = inspector.v.shapeModifiers || [];
                    for (let i = 0; i < list.length; ++i)
                        if (list[i].type === type) return list[i][key];
                    return 0;
                }
                Label { text: "Shape"; color: win.text; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Smooth ×"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; decimals: 0; value: parent.parent.amount("smooth", "times"); onCommitted: function (n) { backend.setShapeModifier("smooth", Math.round(n)) } }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "Simplify to"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField { Layout.fillWidth: true; decimals: 0; value: parent.parent.amount("simplify", "cells"); onCommitted: function (n) { backend.setShapeModifier("simplify", Math.round(n)) } }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "Smooth relaxes the AI's lumps; simplify keeps about that many points across it. 0 for none. The shape itself stays as made."
                }
            }

            // A drawing's pen.
            RowLayout {
                visible: backend.selected >= 0 && !!inspector.v.drawing
                Layout.fillWidth: true
                spacing: 8
                Label { text: "Pen colour"; color: win.text; Layout.preferredWidth: 90 }
                Rectangle { width: 18; height: 18; radius: 3; color: backend.drawColour; border.color: win.line }
                SmallField {
                    id: penField
                    Layout.fillWidth: true
                    Component.onCompleted: text = backend.drawColour
                    Connections { target: backend; function onToolChanged() { if (!penField.activeFocus) penField.text = backend.drawColour } }
                    onAccepted: { backend.drawColour = text; inspector.done() }
                }
                ToolButton { text: "Draw"; font.pixelSize: 12; onClicked: { backend.startDrawing(); inspector.done() } }
            }

            // Rigid bodies, as Blender's: they fall, land and knock about.
            ColumnLayout {
                visible: backend.selected >= 0 && !inspector.isParticles && !inspector.v.empty
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 2
                readonly property var body: inspector.v.body || {}
                readonly property bool isBody: body.mass !== undefined
                Label { text: "Physics"; color: win.text; font.bold: true }
                RowLayout {
                    Layout.fillWidth: true
                    CheckBox {
                        text: "Body"
                        checked: parent.parent.isBody
                        onClicked: { backend.setBody(checked, false, .3); inspector.done() }
                    }
                    CheckBox {
                        text: "Fixed"
                        enabled: parent.parent.isBody
                        checked: !!parent.parent.body.fixed
                        onClicked: { backend.setBody(true, checked, parent.parent.body.bounce === undefined ? .3 : parent.parent.body.bounce); inspector.done() }
                    }
                    CheckBox {
                        text: "Tumble"
                        enabled: parent.parent.isBody
                        checked: parent.parent.body.tumble !== false
                        onClicked: { backend.setBodyValue("tumble", checked); inspector.done() }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: parent.isBody
                    spacing: 8
                    Label { text: "Bounce"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField {
                        Layout.fillWidth: true
                        value: parent.parent.body.bounce === undefined ? .3 : parent.parent.body.bounce
                        onCommitted: function (n) { backend.setBody(true, !!parent.parent.body.fixed, n) }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: parent.isBody
                    spacing: 8
                    Label { text: "Friction"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField {
                        Layout.fillWidth: true
                        value: parent.parent.body.friction === undefined ? .5 : parent.parent.body.friction
                        onCommitted: function (n) { backend.setBodyValue("friction", Math.max(0, Math.min(1, n))) }
                    }
                }
                ToolButton {
                    visible: parent.isBody
                    text: "Simulate from the playhead"
                    font.pixelSize: 12
                    onClicked: { backend.simulate(); inspector.done() }
                }
            }

            // Saved motions laid on it as clips.
            ColumnLayout {
                visible: (inspector.v.clips || []).length > 0
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 2
                Label { text: "Clips"; color: win.text; font.bold: true }
                Repeater {
                    model: inspector.v.clips || []
                    delegate: ColumnLayout {
                        id: clipRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                color: win.text
                                font.pixelSize: 12
                                text: modelData.motion + " from " + Number(modelData.start || 0).toFixed(2) + " s"
                                      + ((modelData.repeat || 1) > 1 ? ", ×" + modelData.repeat : "")
                            }
                            ComboBox {
                                font.pixelSize: 12
                                implicitHeight: 30
                                Layout.preferredWidth: 96
                                model: ["add", "replace"]
                                currentIndex: modelData.blend === "replace" ? 1 : 0
                                onActivated: function (i) { backend.setClip(index, "blend", model[i]); inspector.done() }
                            }
                            ToolButton { text: "✕"; font.pixelSize: 12; implicitHeight: 26; onClicked: { backend.removeClip(index); inspector.done() } }
                        }
                        // Influence and fades, so one clip hands over to the next.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Repeater {
                                model: [["amount", "Influence", 1], ["fadeIn", "Fade in s", 0], ["fadeOut", "Fade out s", 0]]
                                delegate: NumberField {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    ToolTip.visible: hovered
                                    ToolTip.text: modelData[1]
                                    value: clipRow.modelData[modelData[0]] === undefined ? modelData[2] : clipRow.modelData[modelData[0]]
                                    onCommitted: function (n) { backend.setClip(clipRow.index, modelData[0], n) }
                                }
                            }
                        }
                    }
                }
            }

            // The world's own look, with nothing else selected.
            ColumnLayout {
                visible: !inspector.hasLayer && backend.world.active
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 4
                readonly property var w: backend.worldInfo
                WorldColour { name: "hazeColor"; label: "Haze colour"; value: parent.w.hazeColor || "" }
                WorldNumber { name: "hazeDensity"; label: "Haze a meter"; value: parent.w.hazeDensity || 0; decimals: 3 }
                WorldNumber { name: "grain"; label: "Grain"; value: parent.w.grain || 0; decimals: 3 }
                WorldNumber { name: "shutter"; label: "Shutter°"; value: parent.w.shutter || 0; decimals: 0 }
                WorldColour { name: "dust"; label: "Dust colour"; value: parent.w.dust || "" }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "The camera stands " + Number(parent.w.height || 0).toFixed(2) + " m above the ground; the world is "
                          + Number(parent.w.scale || 1).toFixed(2) + "× as the AI read it (a piece's Really tall sets it). "
                          + "Haze hides what's moved farther off than the picture had it; the shutter blurs what moves (0 for none)."
                }
            }
            Label {
                visible: !inspector.hasLayer && !backend.world.active
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
                    text: backend.world.active
                          ? "In the world: Zoom and Rotation work as a lens and a tilt; move and turn it below."
                          : "X and Y are the point of the picture at the middle of the view. Layers follow the camera as much as their Follow camera says."
                }
                ColumnLayout {
                    visible: backend.world.active
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: "In the world"; color: win.text; font.bold: true; Layout.topMargin: 6 }
                    PropertyRow { name: "worldX"; label: "Right m" }
                    PropertyRow { name: "worldY"; label: "Up m" }
                    PropertyRow { name: "worldZ"; label: "Ahead m" }
                    PropertyRow { name: "yaw"; label: "Turn right°" }
                    PropertyRow { name: "pitch"; label: "Look up°" }
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
                // An emitter: from a layer, where it goes, rather than all over.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { width: 14; height: 1 }
                    Label { text: "From"; color: win.text; Layout.preferredWidth: 90 }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        implicitHeight: 34
                        model: {
                            const names = ["everywhere"];
                            for (let i = backend.layers.length - 1; i >= 0; --i)
                                if (backend.layers[i].name !== inspector.v.name && !backend.layers[i].particles)
                                    names.push(backend.layers[i].name);
                            return names;
                        }
                        currentIndex: Math.max(0, model.indexOf(inspector.isParticles ? inspector.v.particles.from || "everywhere" : "everywhere"))
                        onActivated: function (i) { backend.setParticle("from", i === 0 ? "" : model[i]); inspector.done() }
                    }
                }
                ColumnLayout {
                    visible: inspector.isParticles && !!inspector.v.particles.from
                    Layout.fillWidth: true
                    spacing: 4
                    ParticleRow { name: "rate"; label: "A second" }
                    ParticleRow { name: "life"; label: "Life s"; decimals: 2 }
                    ParticleRow { name: "inherit"; label: "Its speed ×"; decimals: 2 }
                    ParticleRow { name: "gravity"; label: "Gravity px/s²" }
                    ParticleRow { name: "wind"; label: "Wind px/s²" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: win.muted
                        font.pixelSize: 12
                        text: "They come from its pivot (\"at\" in scene.json picks another point), carry on with some of its speed, and fall or rise. Link its opacity to switch them on and off."
                    }
                }
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
                PropertyRow { name: "z"; label: "Depth z" }
                PropertyRow { name: "scale"; label: "Scale"; decimals: 3 }
                PropertyRow { name: "scaleX"; label: "Stretch X"; decimals: 3 }
                PropertyRow { name: "scaleY"; label: "Stretch Y"; decimals: 3 }
                PropertyRow { name: "rotation"; label: "Rotation°" }
                PropertyRow { name: "opacity"; label: "Opacity %"; factor: 100; decimals: 0 }
            }

            // Where a layer of the world is, in meters and degrees.
            ColumnLayout {
                visible: inspector.hasLayer && !backend.cameraSelected && backend.world.active && !!inspector.v.inWorld
                Layout.fillWidth: true
                spacing: 4
                Label { text: "In the world"; color: win.text; font.bold: true; Layout.topMargin: 8 }
                PropertyRow { name: "worldX"; label: "Right m" }
                PropertyRow { name: "worldY"; label: "Up m" }
                PropertyRow { name: "worldZ"; label: "Ahead m" }
                PropertyRow { name: "yaw"; label: "Turn right°" }
                PropertyRow { name: "pitch"; label: "Nose up°" }
                PropertyRow { name: "roll"; label: "Lean right°" }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "From where it is in the picture. Drag it to move it, Shift+drag for nearer and farther. Its X, Y and Scale above are flat and don't move it in the world."
                }

                // Real size, and a drive over the ground.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 8
                    Item { width: 14; height: 1 }
                    Label { text: "Really tall m"; color: win.text; Layout.preferredWidth: 90 }
                    NumberField {
                        id: tallField
                        Layout.fillWidth: true
                        decimals: 2
                        value: 1.4
                        onCommitted: function (n) { value = n; backend.setRealSize(n) }
                    }
                }
                Label { text: "Drive"; color: win.text; font.bold: true; Layout.topMargin: 6 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Item { width: 14; height: 1 }
                    Label { text: "From"; color: win.text; Layout.preferredWidth: 40 }
                    NumberField { id: fromX; onCommitted: function (n) { value = n }; Layout.fillWidth: true; value: 8; decimals: 1; ToolTip.text: "meters right"; ToolTip.visible: hovered }
                    NumberField { id: fromAhead; onCommitted: function (n) { value = n }; Layout.fillWidth: true; value: 22; decimals: 1; ToolTip.text: "meters ahead"; ToolTip.visible: hovered }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Item { width: 14; height: 1 }
                    Label { text: "To"; color: win.text; Layout.preferredWidth: 40 }
                    NumberField { id: toX; onCommitted: function (n) { value = n }; Layout.fillWidth: true; value: -5; decimals: 1; ToolTip.text: "meters right"; ToolTip.visible: hovered }
                    NumberField { id: toAhead; onCommitted: function (n) { value = n }; Layout.fillWidth: true; value: -1; decimals: 1; ToolTip.text: "meters ahead"; ToolTip.visible: hovered }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Item { width: 14; height: 1 }
                    Label { text: "km/h"; color: win.text; Layout.preferredWidth: 40 }
                    NumberField { id: kmh; onCommitted: function (n) { value = n }; Layout.fillWidth: true; value: 50; decimals: 0 }
                    CheckBox { id: faceWay; text: "Face the way"; font.pixelSize: 12 }
                }
                Button {
                    Layout.fillWidth: true
                    text: "Drive it from the playhead"
                    onClicked: {
                        backend.driveSelected(fromX.value, fromAhead.value, toX.value, toAhead.value, kmh.value, faceWay.checked);
                        inspector.done();
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.muted
                    font.pixelSize: 12
                    text: "Places in meters, right of the camera and ahead of it. It keeps to the ground, flies off crests and lands, with dust and a shadow. S gives it a 3D shape first."
                }
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
                      ? "On a keyframe · E changes its easing; drag the curve's handles, or type one: "
                        + "spring(3) settles, bounce(2) lands, arc(200) jumps"
                      : "K keyframes every property here"
            }
            RowLayout {
                visible: inspector.hasLayer && !!inspector.v.keyEase
                Layout.fillWidth: true
                spacing: 8
                Item { width: 14; height: 1 }
                Label { text: "Easing"; color: win.text; Layout.preferredWidth: 90 }
                SmallField {
                    id: easeField
                    Layout.fillWidth: true
                    property string value: inspector.v.keyEase || ""
                    onValueChanged: if (!activeFocus) text = value
                    onActiveFocusChanged: if (!activeFocus) text = value
                    Component.onCompleted: text = value
                    Keys.onEscapePressed: inspector.done()
                    onAccepted: { backend.setEase(text); inspector.done() }
                }
            }

            // The easing out of the keyframe here, as a curve: drag its two
            // handles to shape it. Up past the top overshoots.
            Item {
                id: curve
                visible: inspector.hasLayer && !!inspector.v.keyEase && (inspector.v.keyCurve || []).length === 4
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

            // The look over the whole frame, as a film's grade.
            Label { text: "Look"; color: win.text; font.bold: true }
            Repeater {
                model: [["exposure", "Exposure", 2, 0], ["contrast", "Contrast", 2, 1], ["saturation", "Saturation", 2, 1],
                        ["temperature", "Warmth", 2, 0], ["tint", "Tint", 2, 0], ["vignette", "Vignette", 2, 0],
                        ["bloom", "Bloom", 2, 0], ["bloomThreshold", "Bloom from", 2, .75],
                        ["focus", backend.world.active ? "Focus m" : "Focus z", 1, 0], ["aperture", "Aperture", 1, 0]]
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: modelData[1]; color: win.text; Layout.preferredWidth: 90 }
                    NumberField {
                        Layout.fillWidth: true
                        decimals: modelData[2]
                        value: (backend.sceneInfo.look || {})[modelData[0]] !== undefined ? backend.sceneInfo.look[modelData[0]] : modelData[3]
                        onCommitted: function (n) { backend.setLook(modelData[0], n) }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.muted
                font.pixelSize: 12
                text: "Exposure in stops; warmth and tint from -1 to 1; bloom makes what's brighter than \"Bloom from\" glow. An aperture blurs what's away from the focus" + (backend.world.active ? " (meters from the camera)." : " (a layer's Depth z).")
            }
            Rectangle { Layout.fillWidth: true; Layout.topMargin: 10; Layout.bottomMargin: 4; height: 1; color: win.line }

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
            SceneRow { name: "vanishX"; label: "Vanish X"; decimals: 0 }
            SceneRow { name: "vanishY"; label: "Vanish Y"; decimals: 0 }
            SceneRow { name: "distance"; label: "Distance"; decimals: 0 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.muted
                font.pixelSize: 12
                text: "A layer's Depth z sets it back: it shrinks toward the vanishing point, where the road meets the horizon, half size at z = Distance."
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
