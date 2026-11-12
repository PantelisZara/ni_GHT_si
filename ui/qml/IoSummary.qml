import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: summary
    required property var backend
    spacing: 12

    function quality(value) {
        return String(value).replace(/_/g, " ");
    }
    function reading(field, scale, suffix) {
        const present = field && field.quality === "valid" && field.value !== undefined
            && field.value !== null && isFinite(Number(field.value));
        return (present ? (Number(field.value) / scale).toFixed(1) + suffix : "—")
            + " (" + quality(field ? field.quality : "warming") + ")";
    }

    Frame {
        objectName: "ioRatesSection"
        Layout.fillWidth: true
        padding: 16
        background: Rectangle { color: "#15181c"; radius: 7; border.color: "#292e35" }
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Label { text: "Network and disk throughput"; font.weight: Font.DemiBold }
            Label {
                objectName: "ioStatusText"
                color: "#a5afb9"
                font.pixelSize: 12
                text: "I/O: " + summary.quality(summary.backend.ioQuality)
                    + " · age " + (summary.backend.ioAgeMs < 0 ? "—" : summary.backend.ioAgeMs + " ms")
                    + " · diagnostics " + summary.backend.ioIssueCount
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Accessible.name: text
            }
            ToolButton {
                id: ioDiagnosticsToggle
                objectName: "ioDiagnosticsToggle"
                visible: summary.backend.ioMessage.length > 0
                checkable: true
                text: checked ? "Hide diagnostics" : "Show diagnostics"
            }
            Label {
                visible: ioDiagnosticsToggle.checked && summary.backend.ioMessage.length > 0
                text: summary.backend.ioMessage
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                Layout.fillWidth: true
            }
            Label { text: "Network interfaces"; font.weight: Font.DemiBold }
            Label { visible: summary.backend.networkRates.length === 0; text: "No interface readings in the latest usable sample."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Repeater {
                model: summary.visible ? summary.backend.networkRates.length : 0
                delegate: ColumnLayout {
                    required property int index
                    readonly property var readingRow: index >= 0 && index < summary.backend.networkRates.length
                        ? summary.backend.networkRates[index] : ({})
                    objectName: "networkRow_" + index
                    Layout.fillWidth: true
                    spacing: 4
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#292e35" }
                    Label { text: readingRow.name || ""; font.weight: Font.DemiBold; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Label {
                        text: "Receive: " + summary.reading(readingRow.readBytesPerSecond, 1024, " KiB/s")
                            + " · transmit: " + summary.reading(readingRow.writtenBytesPerSecond, 1024, " KiB/s")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
            Label { text: "Block devices"; font.weight: Font.DemiBold }
            Label { visible: summary.backend.diskRates.length === 0; text: "No device readings in the latest usable sample."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Repeater {
                model: summary.visible ? summary.backend.diskRates.length : 0
                delegate: ColumnLayout {
                    required property int index
                    readonly property var readingRow: index >= 0 && index < summary.backend.diskRates.length
                        ? summary.backend.diskRates[index] : ({})
                    objectName: "diskRow_" + index
                    Layout.fillWidth: true
                    spacing: 4
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#292e35" }
                    Label { text: readingRow.name || ""; font.weight: Font.DemiBold; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Label {
                        text: "Read: " + summary.reading(readingRow.readBytesPerSecond, 1048576, " MiB/s")
                            + " · write: " + summary.reading(readingRow.writtenBytesPerSecond, 1048576, " MiB/s")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
            Label {
                color: "#a5afb9"
                font.pixelSize: 12
                text: "Rates cover actual counter intervals. Interfaces and devices can overlap; values are not summed. Partitions without a stable lifetime identity show unavailable rates."
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }
    Frame {
        objectName: "capacitySection"
        Layout.fillWidth: true
        padding: 16
        background: Rectangle { color: "#15181c"; radius: 7; border.color: "#292e35" }
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Label { text: "Selected filesystems"; font.weight: Font.DemiBold }
            Label {
                objectName: "capacityStatusText"
                color: "#a5afb9"
                font.pixelSize: 12
                text: "Capacity: " + summary.quality(summary.backend.capacityQuality)
                    + " · age " + (summary.backend.capacityAgeMs < 0 ? "—" : summary.backend.capacityAgeMs + " ms")
                    + " · diagnostics " + summary.backend.capacityIssueCount
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Accessible.name: text
            }
            ToolButton {
                id: capacityDiagnosticsToggle
                objectName: "capacityDiagnosticsToggle"
                visible: summary.backend.capacityMessage.length > 0
                checkable: true
                text: checked ? "Hide diagnostics" : "Show diagnostics"
            }
            Label {
                visible: capacityDiagnosticsToggle.checked && summary.backend.capacityMessage.length > 0
                text: summary.backend.capacityMessage
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                Layout.fillWidth: true
            }
            Label { visible: summary.backend.filesystemCapacity.length === 0; text: "No capacity readings in the latest usable sample."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Repeater {
                model: summary.visible ? summary.backend.filesystemCapacity.length : 0
                delegate: ColumnLayout {
                    required property int index
                    readonly property var readingRow: index >= 0 && index < summary.backend.filesystemCapacity.length
                        ? summary.backend.filesystemCapacity[index] : ({})
                    objectName: "capacityRow_" + index
                    Layout.fillWidth: true
                    spacing: 4
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#292e35" }
                    Label { text: readingRow.path || ""; font.weight: Font.DemiBold; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Label {
                        text: "Used: " + summary.reading(readingRow.usedBytes, 1073741824, " GiB")
                            + " · total: " + summary.reading(readingRow.totalBytes, 1073741824, " GiB")
                            + " · available: " + summary.reading(readingRow.availableBytes, 1073741824, " GiB")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
            Label { color: "#a5afb9"; font.pixelSize: 12; text: "Available space excludes reserved blocks; used plus available need not equal total. Only configured paths are sampled."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }
}
