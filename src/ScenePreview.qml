import QtQuick

// One live copy of the scene at one moment, scaled to fit. It never mirrors,
// whatever the language, so it looks exactly like the render.
Item {
    id: root

    property var component: null
    property real time: 0
    property string format: "16:9"
    property var scene: null
    property string problem: ""
    property bool outlined: false

    readonly property size frameSize: backend.formatSize(format)
    readonly property real frameScale: Math.min(width / frameSize.width, height / frameSize.height)

    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    // An edge around the frame, so a scene as dark as the window still shows its size.
    Rectangle {
        anchors.fill: frame
        anchors.margins: -1
        visible: root.outlined
        color: "transparent"
        border.color: "#2c2c33"
    }

    Rectangle {
        id: frame
        anchors.centerIn: parent
        width: root.frameSize.width * root.frameScale
        height: root.frameSize.height * root.frameScale
        color: "black"
        clip: true

        Item {
            id: holder
            width: root.frameSize.width
            height: root.frameSize.height
            scale: root.frameScale
            transformOrigin: Item.TopLeft
        }
    }

    // The topmost visible text at a point of the frame, in the format's pixels,
    // so a note can say what it's about.
    function textAt(px, py) {
        if (!scene)
            return ""
        let found = ""
        function visit(item, opacity) {
            if (!item.visible)
                return
            opacity *= item.opacity
            if (opacity < 0.05)
                return
            if (typeof item.text === "string" && item.font !== undefined && item.text.trim() !== "") {
                const local = item.mapFromItem(scene, px, py)
                if (local.x >= 0 && local.y >= 0 && local.x <= item.width && local.y <= item.height)
                    found = item.text
            }
            for (let i = 0; i < item.children.length; ++i)
                visit(item.children[i], opacity)
        }
        visit(scene, 1)
        return found.replace(/<[^>]*>/g, "").replace(/&nbsp;/g, " ").replace(/\s+/g, " ").trim()
    }

    function rebuild() {
        if (scene) {
            scene.destroy()
            scene = null
        }
        problem = ""
        if (!component || component.status !== Component.Ready)
            return
        // The format's size, asked for directly: this runs from onFormatChanged,
        // possibly before frameSize's binding has caught up.
        const size = backend.formatSize(format)
        const created = component.createObject(holder, { width: size.width, height: size.height })
        if (!created) {
            problem = component.errorString() || qsTr("The scene could not be created; its error is below.")
            return
        }
        if (created.format !== undefined)
            created.format = format
        if (created.t === undefined)
            problem = qsTr("Declare “property real t”, the time in seconds that omotion sets for every frame.")
        else if (!(created.duration > 0))
            problem = qsTr("Declare “property real duration” in seconds, above 0.")
        else
            created.t = Qt.binding(() => root.time)
        scene = created
    }

    onComponentChanged: rebuild()
    onFormatChanged: rebuild()
    Component.onCompleted: rebuild()
    Component.onDestruction: if (scene) scene.destroy()
}
