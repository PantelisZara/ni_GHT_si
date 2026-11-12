import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: plot
    required property var points
    required property string metric
    required property string title
    property real upperBound: 100
    color: "#15181c"
    radius: 7
    border.color: "#292e35"
    implicitHeight: 220
    onPointsChanged: if (visible) graph.requestPaint()
    onVisibleChanged: if (visible) graph.requestPaint()
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8
        Label { text: plot.title; font.weight: Font.DemiBold }
        Label { text: plot.upperBound.toFixed(1); color: "#a5afb9"; font.pixelSize: 11 }
        Canvas {
            id: graph
            objectName: "historyCanvas"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Accessible.name: plot.title + ". " + plot.points.length + " samples."
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d");
                ctx.clearRect(0, 0, width, height);
                ctx.strokeStyle = "#292f36";
                ctx.lineWidth = 1;
                ctx.beginPath();
                for (let line = 0; line <= 4; ++line) {
                    const y = Math.round(height * line / 4) + 0.5;
                    ctx.moveTo(0, y);
                    ctx.lineTo(width, y);
                }
                ctx.stroke();
                if (plot.points.length < 2 || plot.upperBound <= 0) return;
                let first = undefined;
                let last = undefined;
                for (let i = 0; i < plot.points.length; ++i) {
                    const time = plot.points[i].timeMs;
                    if (typeof time === "number" && isFinite(time)) {
                        if (first === undefined) first = time;
                        last = time;
                    }
                }
                if (first === undefined || last === undefined) return;
                const span = last - first;
                if (span <= 0) return;
                ctx.strokeStyle = "#8bbce8";
                ctx.lineWidth = 1.5;
                ctx.beginPath();
                let connected = false;
                for (let i = 0; i < plot.points.length; ++i) {
                    const point = plot.points[i];
                    const value = point[plot.metric];
                    if (typeof point.timeMs !== "number" || !isFinite(point.timeMs)
                        || typeof value !== "number" || !isFinite(value)) {
                        connected = false;
                        continue;
                    }
                    const x = (point.timeMs - first) / span * width;
                    const y = height * (1 - Math.min(1, Math.max(0, value / plot.upperBound)));
                    if (connected) ctx.lineTo(x, y);
                    else ctx.moveTo(x, y);
                    connected = true;
                }
                ctx.stroke();
            }
        }
        Label { text: "0 · oldest → newest"; color: "#a5afb9"; font.pixelSize: 11 }
    }
}
