import QtQuick
import Omotion

// Jotter sync announcement. Built from shotlist.md.
Rectangle {
    id: scene
    property real t: 0
    property real duration: 8
    property int fps: 30
    property string format: "16:9"
    property var formats: ["16:9"]
    property var beats: [
        { at: 0.0, name: "Hook" },
        { at: 3.0, name: "Detail" },
        { at: 5.5, name: "Close" }
    ]
    readonly property real unit: Math.min(width, height) / 1080
    width: 1920
    height: 1080
    color: "#f4f1ea"

    // Hook: the claim.
    Text {
        id: claim
        readonly property real arrive: Motion.spring(scene.t, 0.35, 220, 24)
        x: 140 * scene.unit
        y: 380 * scene.unit
        text: "Jotter now syncs."
        color: "#16181d"
        font.pixelSize: 140 * scene.unit
        font.weight: Font.Bold
        opacity: Motion.clamp(arrive, 0, 1) * (1 - Motion.progress(scene.t, 2.8, 3.0))
        transform: Translate { y: (1 - claim.arrive) * 40 * scene.unit }
    }

    // Detail: where it syncs, and the badge.
    Item {
        anchors.fill: parent
        opacity: Motion.progress(scene.t, 3.0, 3.3) * (1 - Motion.progress(scene.t, 5.4, 5.5))
        visible: scene.t > 2.95 && scene.t < 5.55
        Text {
            x: 140 * scene.unit
            y: 360 * scene.unit
            text: "Every note, everywhere."
            color: "#16181d"
            font.pixelSize: 110 * scene.unit
            font.weight: Font.Bold
        }
        Text {
            x: 144 * scene.unit
            y: 520 * scene.unit
            text: "Syncs across your phone, tablet and laptop in real time."
            color: "#666b74"
            font.pixelSize: 30 * scene.unit
        }
        Image {
            // The real sync settings screen.
            x: 1560 * scene.unit
            y: 640 * scene.unit
            width: 200 * scene.unit
            height: 356 * scene.unit
            source: "assets/screens/sync-settings.png"
            smooth: true
        }
        Rectangle {
            x: 144 * scene.unit
            y: 600 * scene.unit
            width: 180 * scene.unit
            height: 64 * scene.unit
            radius: 32 * scene.unit
            color: "#ff5a1f"
            scale: Motion.spring(scene.t, 5.05, 300, 22)
            Text {
                anchors.centerIn: parent
                text: "Free"
                color: "white"
                font.pixelSize: 36 * scene.unit
                font.weight: Font.Bold
            }
        }
    }

    // Close: the end card.
    Column {
        anchors.centerIn: parent
        spacing: 24 * scene.unit
        opacity: Motion.progress(scene.t, 5.6, 6.0)
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Update Jotter today"
            color: "#16181d"
            font.pixelSize: 96 * scene.unit
            font.weight: Font.Bold
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "jotter.app"
            color: "#666b74"
            font.pixelSize: 56 * scene.unit
        }
    }
}
