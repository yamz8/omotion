import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The person's notes, each pinned to a moment, and what the agent has done.
// Notes live in notes.md, so the agent reads them where it reads everything else.
Rectangle {
    id: panel
    color: "#121215"

    property string selected: ""
    signal noteSelected(string id)

    readonly property int openCount: {
        let count = 0
        for (const note of backend.notes)
            if (!note.done)
                ++count
        return count
    }

    Rectangle {
        width: 1
        height: parent.height
        color: "#24242a"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: panel.openCount > 0 ? qsTr("Notes · %n open", "", panel.openCount) : qsTr("Notes")
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Button {
                Material.roundedScale: theme.radius
                text: qsTr("Copy for Agent")
                flat: true
                font.pixelSize: 12
                focusPolicy: Qt.NoFocus
                enabled: panel.openCount > 0
                onClicked: backend.copyOpenNotes()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Copies a request to fix the open notes, to paste into your agent's terminal.")
                ToolTip.delay: 400
            }
        }

        Label {
            Layout.fillWidth: true
            visible: backend.notes.length === 0
            wrapMode: Text.WordWrap
            text: qsTr("Click the film to pin a note to that moment. The agent fixes each one with evidence, or replies.")
            color: "#8d8d95"
            font.pixelSize: 12
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 3
            clip: true
            spacing: 8
            model: backend.notes
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: card
                required property var modelData
                width: list.width
                height: cardColumn.implicitHeight + 20
                radius: Math.min(4, theme.radius)
                color: panel.selected === modelData.id ? "#22161c" : "#18181c"
                border.color: panel.selected === modelData.id ? "#FF2D78" : "#26262c"
                opacity: modelData.done ? 0.6 : 1

                ColumnLayout {
                    id: cardColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: card.modelData.done ? "transparent" : "#FF2D78"
                            border.width: 2
                            border.color: card.modelData.done ? "#6d6d75" : "#FF2D78"
                        }
                        Label {
                            text: card.modelData.id
                            font.pixelSize: 12
                            font.family: "monospace"
                            color: "#9a9aa2"
                        }
                        Label {
                            Layout.fillWidth: true
                            text: (translator.language, qsTr("%1 s · %2").arg(card.modelData.at.toLocaleString(Qt.locale(), 'f', 2))
                                                                     .arg(card.modelData.format))
                            font.pixelSize: 12
                            color: "#c9c9cf"
                            font.underline: timeHover.hovered
                            HoverHandler { id: timeHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: panel.noteSelected(card.modelData.id)
                            }
                        }
                        ToolButton {
                            text: qsTr("Copy")
                            font.pixelSize: 11
                            focusPolicy: Qt.NoFocus
                            implicitHeight: 28
                            onClicked: backend.copyNote(card.modelData.id)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: card.modelData.text
                        wrapMode: Text.WordWrap
                        color: "#f2f2f2"
                        font.strikeout: card.modelData.done
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: card.modelData.target !== "" || (card.modelData.beat !== "" && card.modelData.beat !== "no beat")
                        text: card.modelData.target !== "" ? qsTr("On “%1”").arg(card.modelData.target)
                                                           : qsTr("Beat: %1").arg(card.modelData.beat)
                        elide: Text.ElideRight
                        font.pixelSize: 11
                        color: "#8d8d95"
                    }
                    Repeater {
                        model: card.modelData.answers
                        delegate: Label {
                            required property string modelData
                            Layout.fillWidth: true
                            text: modelData
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: modelData.startsWith("fixed:") ? theme.accent : "#c9c9cf"
                        }
                    }
                    // The agent's answer comes first, since it's what changes; the
                    // evidence is a thumbnail that opens full size.
                    Image {
                        Layout.preferredWidth: Math.min(parent.width, 200)
                        Layout.preferredHeight: visible ? Layout.preferredWidth * sourceSize.height / Math.max(1, sourceSize.width) : 0
                        visible: card.modelData.evidenceUrl != ""
                        source: card.modelData.evidenceUrl != "" ? card.modelData.evidenceUrl + "?" + card.modelData.evidenceStamp : ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: backend.openStudioPath(card.modelData.evidence) }
                    }
                }
            }
        }

        Label {
            Layout.topMargin: 6
            text: qsTr("Activity")
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1
            color: "#7d7d85"
        }

        ListView {
            id: activityList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 2
            clip: true
            spacing: 6
            model: backend.activity
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Label {
                required property string modelData
                required property int index
                width: activityList.width
                text: modelData
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: index === 0 ? "#e6e6ea" : "#8d8d95"
            }
        }

        Label {
            visible: backend.activity.length === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("The agent's steps appear here as it writes them to activity.md.")
            color: "#8d8d95"
            font.pixelSize: 12
        }
    }
}
