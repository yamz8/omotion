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
    readonly property bool hasLayer: backend.selected >= 0

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
        Label { text: row.label; color: win.text; Layout.preferredWidth: 70 }
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
                visible: inspector.hasLayer
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

            ColumnLayout {
                visible: inspector.hasLayer
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
                visible: inspector.hasLayer
                Layout.topMargin: 6
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: inspector.v.keyEase ? theme.accent : win.muted
                text: inspector.v.keyEase
                      ? "On a keyframe · eases " + inspector.v.keyEase + " (E changes)"
                      : "K keyframes every property here"
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
        }
    }
}
