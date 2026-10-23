import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Writing a note about one moment. It opens where the person clicked, and says
// exactly what the note will be pinned to.
Popup {
    id: editor

    property real at: 0
    property string format: ""
    property string beat: ""
    property point point: Qt.point(0, 0)
    property string target: ""

    width: 360
    padding: 14
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape
    Material.roundedScale: theme.radius

    background: Rectangle {
        radius: theme.radius
        color: "#1d1d22"
        border.color: "#FF2D78"
    }

    function save() {
        if (field.text.trim() !== "")
            backend.addNote(at, format, beat, Math.round(point.x), Math.round(point.y), target, field.text)
        close()
    }

    onOpened: {
        field.text = ""
        field.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: (translator.language,
                   qsTr("%1 s · %2").arg(editor.at.toLocaleString(Qt.locale(), 'f', 2)).arg(editor.format)
                   + (editor.beat !== "" ? " · " + editor.beat : ""))
            font.pixelSize: 12
            color: "#c9c9cf"
        }
        Label {
            Layout.fillWidth: true
            visible: editor.target !== ""
            text: qsTr("On “%1”").arg(editor.target)
            elide: Text.ElideRight
            font.pixelSize: 12
            color: "#8d8d95"
        }
        // A Material TextArea floats its placeholder up as a label, so it gets
        // room of its own above the field.
        TextArea {
            id: field
            Layout.topMargin: 10
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(72, implicitHeight)
            placeholderText: qsTr("What should change here?")
            wrapMode: TextEdit.Wrap
            Keys.onReturnPressed: event => {
                if (event.modifiers & Qt.ShiftModifier)
                    event.accepted = false
                else
                    editor.save()
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button {
                Material.roundedScale: theme.radius
                text: qsTr("Cancel")
                flat: true
                onClicked: editor.close()
            }
            Button {
                Material.roundedScale: theme.radius
                text: qsTr("Pin Note")
                highlighted: true
                enabled: field.text.trim() !== ""
                onClicked: editor.save()
            }
        }
    }
}
