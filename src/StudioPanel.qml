import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The studio's state as the agent leaves it in files: the gates, the assets,
// and the evidence, newest first. Everything here opens what it names.
Rectangle {
    id: panel
    color: "#121215"

    readonly property int currentGate: {
        for (let i = 0; i < backend.gates.length; ++i)
            if (!backend.gates[i].passed)
                return backend.gates[i].number
        return 0
    }

    // The files that are each gate's evidence.
    function gateFile(number) {
        return ["", "brief.md", "shotlist.md", "reviews", "reviews", "reviews", "renders"][number] || ""
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: "#24242a"
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 16
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            width: parent.width
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: backend.studioName
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                ToolButton {
                    text: qsTr("Folder")
                    font.pixelSize: 12
                    focusPolicy: Qt.NoFocus
                    onClicked: backend.openStudioPath(backend.studioRoot)
                }
            }

            SectionLabel { text: qsTr("Gates") }

            Repeater {
                model: backend.gates
                delegate: ItemDelegate {
                    id: gateRow
                    required property var modelData
                    readonly property bool current: modelData.number === panel.currentGate
                    Layout.fillWidth: true
                    Layout.preferredHeight: gateColumn.implicitHeight + 12
                    padding: 6
                    focusPolicy: Qt.NoFocus
                    onClicked: backend.openStudioPath(panel.gateFile(modelData.number))
                    ToolTip.visible: hovered && modelData.status !== ""
                    ToolTip.text: modelData.status
                    ToolTip.delay: 500

                    contentItem: RowLayout {
                        spacing: 10
                        Rectangle {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 3
                            width: 12
                            height: 12
                            radius: 6
                            color: gateRow.modelData.passed ? theme.accent : "transparent"
                            border.width: 2
                            border.color: gateRow.modelData.passed || gateRow.current ? theme.accent : "#4a4a52"
                        }
                        ColumnLayout {
                            id: gateColumn
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: gateRow.modelData.number + "  " + gateRow.modelData.title
                                wrapMode: Text.WordWrap
                                font.pixelSize: 13
                                color: gateRow.modelData.passed || gateRow.current ? "#f2f2f2" : "#8d8d95"
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: gateRow.modelData.waiting || (gateRow.current && gateRow.modelData.person)
                                text: gateRow.modelData.waiting ? qsTr("Waiting for your approval") : qsTr("You approve this gate")
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                color: gateRow.modelData.waiting ? "#FF2D78" : "#9a9aa2"
                            }
                        }
                    }
                }
            }

            Label {
                visible: backend.gates.length === 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("No gates.md in this folder.")
                color: "#8d8d95"
                font.pixelSize: 12
            }

            SectionLabel { text: qsTr("Assets") }

            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: backend.assets
                    delegate: Rectangle {
                        id: asset
                        required property var modelData
                        width: 64
                        height: 64
                        radius: Math.min(4, theme.radius)
                        color: "#1b1b20"
                        border.color: assetHover.hovered ? "#55555d" : "#2c2c33"
                        clip: true

                        Image {
                            anchors.fill: parent
                            anchors.margins: 4
                            visible: asset.modelData.kind === "image"
                            source: visible ? asset.modelData.url : ""
                            sourceSize: Qt.size(128, 128)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        FontLoader {
                            id: assetFont
                            source: asset.modelData.kind === "font" ? asset.modelData.url : ""
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: asset.modelData.kind !== "image"
                            text: asset.modelData.kind === "font" ? "Aa"
                                  : asset.modelData.kind === "audio" ? "♪"
                                  : asset.modelData.name.split(".").pop().toUpperCase()
                            font.family: asset.modelData.kind === "font" ? assetFont.font.family : font.family
                            font.pixelSize: asset.modelData.kind === "font" ? 24 : 13
                            color: "#c9c9cf"
                        }
                        HoverHandler { id: assetHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: backend.openStudioPath("assets/" + asset.modelData.name) }
                        ToolTip.visible: assetHover.hovered
                        ToolTip.text: asset.modelData.name
                        ToolTip.delay: 300
                    }
                }
            }

            Repeater {
                model: backend.missingAssets
                delegate: Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Missing: %1").arg(modelData)
                    color: "#ff8a95"
                    font.pixelSize: 12
                }
            }

            Label {
                visible: backend.assets.length === 0 && backend.missingAssets.length === 0
                text: qsTr("Nothing in assets/ yet.")
                color: "#8d8d95"
                font.pixelSize: 12
            }

            SectionLabel { text: qsTr("Evidence") }

            Repeater {
                model: backend.evidence
                delegate: ItemDelegate {
                    id: proof
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 30
                    padding: 4
                    focusPolicy: Qt.NoFocus
                    onClicked: backend.openStudioPath(modelData.path)
                    contentItem: RowLayout {
                        spacing: 8
                        Label {
                            Layout.preferredWidth: 34
                            text: proof.modelData.kind === "video" ? qsTr("Video")
                                  : proof.modelData.kind === "image" ? qsTr("Image") : qsTr("Notes")
                            font.pixelSize: 10
                            color: proof.modelData.kind === "video" ? theme.accent : "#8d8d95"
                        }
                        Label {
                            Layout.fillWidth: true
                            text: proof.modelData.path
                            elide: Text.ElideMiddle
                            font.pixelSize: 12
                            color: "#c9c9cf"
                        }
                        Label {
                            text: (translator.language, proof.modelData.modified.toLocaleTimeString(Qt.locale(), Locale.ShortFormat))
                            font.pixelSize: 11
                            color: "#6d6d75"
                        }
                    }
                }
            }

            Label {
                visible: backend.evidence.length === 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Contact sheets, stills, reviews, and renders appear here as the agent makes them.")
                color: "#8d8d95"
                font.pixelSize: 12
            }
        }
    }

    component SectionLabel: Label {
        Layout.topMargin: 14
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1
        color: "#7d7d85"
    }
}
