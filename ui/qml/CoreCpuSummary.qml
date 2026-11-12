import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: summary
    objectName: "coreCpuSection"
    required property var backend
    padding: 16
    background: Rectangle { color: "#15181c"; radius: 7; border.color: "#292e35" }

    function quality(value) {
        return String(value).replace(/_/g, " ");
    }
    function reading(field) {
        const present = field && field.quality === "valid" && field.value !== undefined
            && field.value !== null && isFinite(Number(field.value));
        return (present ? Number(field.value).toFixed(1) + " %" : "—")
            + " (" + quality(field ? field.quality : "warming") + ")";
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Label { text: "Per-core CPU"; font.weight: Font.DemiBold }
        Label {
            objectName: "coreCpuStatusText"
            color: "#a5afb9"
            font.pixelSize: 12
            text: summary.backend.coreCpu.length + " shown · " + summary.quality(summary.backend.coreCpuQuality)
                + " · age " + (summary.backend.coreCpuAgeMs < 0 ? "—" : summary.backend.coreCpuAgeMs + " ms")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Accessible.name: text
        }
        Label {
            visible: summary.backend.coreCpuMessage.length > 0
            text: summary.backend.coreCpuMessage
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label { visible: summary.backend.coreCpu.length === 0; text: "No per-core readings in the latest usable sample."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        GridLayout {
            columns: summary.width >= 820 ? 4 : summary.width >= 580 ? 3 : 2
            columnSpacing: 16
            rowSpacing: 16
            Layout.fillWidth: true
            Repeater {
                model: summary.visible ? summary.backend.coreCpu.length : 0
                delegate: ColumnLayout {
                    required property int index
                    readonly property var readingRow: index >= 0 && index < summary.backend.coreCpu.length
                        ? summary.backend.coreCpu[index] : ({})
                    objectName: "coreRow_" + index
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: readingRow.name || ""; font.weight: Font.DemiBold; textFormat: Text.PlainText; Layout.fillWidth: true }
                    Label { text: "Busy: " + summary.reading(readingRow.busyPercent); textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: "I/O wait: " + summary.reading(readingRow.iowaitPercent); color: "#a5afb9"; font.pixelSize: 11; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 3
                        radius: 1.5
                        color: "#2a3139"
                        Rectangle {
                            height: parent.height
                            radius: parent.radius
                            color: "#8bbce8"
                            width: readingRow.busyPercent && readingRow.busyPercent.quality === "valid"
                                && readingRow.busyPercent.value !== undefined && readingRow.busyPercent.value !== null
                                && isFinite(Number(readingRow.busyPercent.value))
                                ? parent.width * Math.min(1, Math.max(0, Number(readingRow.busyPercent.value) / 100)) : 0
                        }
                    }
                }
            }
        }
        Label {
            color: "#a5afb9"
            font.pixelSize: 12
            text: "Percentages cover the sampled counter interval. I/O wait is a kernel accounting estimate; initial or reset counters are unavailable."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
