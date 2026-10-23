import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut, shown with ?.
Rectangle {
    id: help
    color: Qt.rgba(0, 0, 0, 0.72)

    signal closed()

    readonly property var shortcuts: [
        { keys: ["Space"], what: qsTr("Play or pause") },
        { keys: ["←", "→"], what: qsTr("Step one frame") },
        { keys: ["Shift ←", "Shift →"], what: qsTr("Jump one second") },
        { keys: ["[", "]"], what: qsTr("Previous or next beat") },
        { keys: ["Home", "End"], what: qsTr("Start or end") },
        { keys: ["F"], what: qsTr("Next format") },
        { keys: ["C"], what: qsTr("Contact sheet") },
        { keys: ["E"], what: qsTr("Export this format") },
        { keys: ["Shift E"], what: qsTr("Export every format") },
        { keys: ["O"], what: qsTr("Open a scene") },
        { keys: ["N"], what: qsTr("New film") },
        { keys: ["R"], what: qsTr("Reload the scene") },
        { keys: ["P"], what: qsTr("Show or hide the studio panels") },
        { keys: [qsTr("Click")], what: qsTr("Pin a note to that moment") },
        { keys: ["?"], what: qsTr("Show shortcuts") },
        { keys: ["Q"], what: qsTr("Quit") }
    ]

    TapHandler {
        onTapped: help.closed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, layout.implicitWidth + 64)
        height: Math.min(parent.height - 48, layout.implicitHeight + 64)
        radius: theme.radius
        color: "#17171b"
        border.color: "#2c2c33"

        ColumnLayout {
            id: layout
            anchors.centerIn: parent
            spacing: 18

            Label {
                text: qsTr("Keyboard shortcuts")
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            GridLayout {
                columns: 2
                columnSpacing: 28
                rowSpacing: 10

                Repeater {
                    model: help.shortcuts
                    delegate: Row {
                        required property var modelData
                        Layout.row: index
                        Layout.column: 0
                        required property int index
                        spacing: 6
                        Repeater {
                            model: modelData.keys
                            delegate: Rectangle {
                                required property string modelData
                                width: Math.max(28, keyLabel.implicitWidth + 16)
                                height: 26
                                radius: Math.min(4, theme.radius)
                                color: "#24242a"
                                border.color: "#3a3a42"
                                Label {
                                    id: keyLabel
                                    anchors.centerIn: parent
                                    text: modelData
                                    font.pixelSize: 13
                                    font.family: "monospace"
                                }
                            }
                        }
                    }
                }
                Repeater {
                    model: help.shortcuts
                    delegate: Label {
                        required property var modelData
                        required property int index
                        Layout.row: index
                        Layout.column: 1
                        text: modelData.what
                        color: "#c9c9cf"
                    }
                }
            }
        }
    }
}
