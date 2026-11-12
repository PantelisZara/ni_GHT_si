import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    required property var backend
    property var processActions: null
    property string selectedApplicationUnit: ""
    property int currentPage: 0
    property int sortColumn: 0
    property bool sortDescending: false
    width: 1100
    height: 720
    minimumWidth: 760
    minimumHeight: 480
    visible: true
    title: "ni_GHT_si"
    readonly property color surface: "#15181c"
    readonly property color outline: "#292e35"
    readonly property color mutedText: "#a5afb9"
    readonly property color accent: "#8bbce8"
    color: "#0c0e11"
    palette.window: "#0c0e11"
    palette.base: "#121519"
    palette.alternateBase: "#181c21"
    palette.text: "#edf0f3"
    palette.windowText: "#edf0f3"
    palette.button: "#1b2026"
    palette.buttonText: "#edf0f3"
    palette.highlight: "#8bbce8"
    palette.highlightedText: "#0c0e11"
    palette.placeholderText: "#8f9ba7"
    palette.mid: "#343c45"
    palette.dark: "#292e35"
    palette.light: "#343c45"

    function qualityLabel(value) {
        return String(value).replace(/_/g, " ");
    }
    function metric(key, scale, suffix) {
        const reading = root.backend.system[key];
        return reading && reading.quality === "valid" && reading.value !== undefined
            ? Number(reading.value / scale).toFixed(1) + suffix : "—";
    }
    function detailMetric(value, quality, scale, suffix) {
        const available = quality === "valid" && value !== undefined && value !== null && isFinite(Number(value));
        return (available ? (Number(value) / scale).toFixed(1) + suffix : "—")
            + " (" + qualityLabel(quality || "warming") + ")";
    }
    function fieldQuality(key) {
        const reading = root.backend.system[key];
        return reading ? qualityLabel(reading.quality) : "warming";
    }

    component Heading: Label {
        property int level: 1
        font.pointSize: Application.font.pointSize * (level === 1 ? 1.60 : 1.20)
        font.weight: Font.Normal
        textFormat: Text.PlainText
        Accessible.role: Accessible.Heading
    }

    component ActionButton: Button {
        id: control
        hoverEnabled: true
        padding: 10
        topPadding: 6
        bottomPadding: 6
        implicitHeight: 32
        background: Rectangle {
            radius: 5
            color: control.checked ? "#24394b" : control.down ? "#28313a"
                : control.hovered ? "#222930" : "#191e24"
            border.color: control.activeFocus ? root.accent : control.checked ? "#42627e" : root.outline
            opacity: control.enabled ? 1 : 0.45
        }
        contentItem: Text {
            text: control.text
            font: control.font
            color: control.enabled ? "#edf0f3" : "#78838e"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Connections {
        target: root.processActions
        function onChanged() {
            if (root.processActions.awaitingConfirmation && !actionConfirmation.visible) actionConfirmation.open();
            else if (!root.processActions.awaitingConfirmation && actionConfirmation.visible) actionConfirmation.close();
        }
    }
    Dialog {
        id: actionConfirmation
        objectName: "actionConfirmation"
        property var fresh: root.processActions ? root.processActions.candidate : ({})
        anchors.centerIn: parent
        width: Math.min(root.width - 48, 620)
        modal: true
        title: fresh.application ? "End Application — fixed-set SIGTERM"
            : fresh.forceKill ? "Force Kill — SIGKILL" : "End Process — SIGTERM"
        closePolicy: Popup.CloseOnEscape
        onOpened: cancelAction.forceActiveFocus()
        onClosed: if (root.processActions && root.processActions.awaitingConfirmation) root.processActions.cancel()
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: (actionConfirmation.fresh.application ? "Request termination of exactly the listed application members. New processes will not be added. "
                    : actionConfirmation.fresh.forceKill ? "Force Kill cannot run cleanup. " : "Request graceful termination. ")
                    + "Unsaved work may be lost. Confirm this freshly bound process, which may differ from an older table observation."
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(200, root.height * 0.30)
                clip: true
                contentWidth: availableWidth
                TextArea {
                objectName: "actionCandidateDetails"
                width: parent.width
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                textFormat: TextEdit.PlainText
                Accessible.name: "Fresh process confirmation details"
                text: !root.processActions || !root.processActions.awaitingConfirmation ? ""
                    : actionConfirmation.fresh.application ? "Unit: " + actionConfirmation.fresh.unit
                    + " · " + actionConfirmation.fresh.memberCount + " confirmed members\n"
                    + actionConfirmation.fresh.membersText
                    : "Name: " + actionConfirmation.fresh.name + " · PID " + actionConfirmation.fresh.pid
                    + " · UID " + actionConfirmation.fresh.uid + "\nUnit: " + actionConfirmation.fresh.unit
                    + "\nExecutable: " + actionConfirmation.fresh.executable
                    + "\nStart: " + actionConfirmation.fresh.startTicks + " ticks · expires within 30 seconds"
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "Identity is retained with a pidfd. Executable, credentials and membership are checked again before dispatch; they can still change after the final check."
            }
        }
        footer: DialogButtonBox {
            ActionButton {
                id: cancelAction
                objectName: "cancelProcessAction"
                text: "Cancel"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: root.processActions.cancel()
            }
            ActionButton {
                objectName: "confirmProcessAction"
                text: actionConfirmation.fresh.application ? "End these application processes"
                    : actionConfirmation.fresh.forceKill ? "Force Kill this process" : "End this process"
                enabled: root.processActions && root.processActions.awaitingConfirmation && !root.processActions.busy
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: root.processActions.confirm(actionConfirmation.fresh.generation)
            }
        }
    }

    header: ToolBar {
        implicitHeight: 56
        background: Rectangle {
            color: "#101216"
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.outline }
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            Heading { text: "ni_GHT_si"; level: 2; color: "#edf0f3" }
            Item { Layout.fillWidth: true }
            Label { text: "Visible processes: " + root.backend.processes.count; color: root.mutedText }
        }
    }
    footer: ToolBar {
        implicitHeight: 30
        background: Rectangle {
            color: "#101216"
            Rectangle { width: parent.width; height: 1; color: root.outline }
        }
        Label {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 11
            color: root.mutedText
            text: "System: " + root.qualityLabel(root.backend.systemQuality)
                + " · data age " + (root.backend.systemAgeMs < 0 ? "—" : root.backend.systemAgeMs + " ms")
                + "     Processes: " + root.qualityLabel(root.backend.processQuality)
                + " · data age " + (root.backend.processAgeMs < 0 ? "—" : root.backend.processAgeMs + " ms")
            textFormat: Text.PlainText
            elide: Text.ElideRight
        }
    }
    RowLayout {
        anchors.fill: parent
        spacing: 0
        Pane {
            Layout.fillHeight: true
            Layout.preferredWidth: 156
            padding: 12
            background: Rectangle {
                color: "#101216"
                Rectangle { anchors.right: parent.right; height: parent.height; width: 1; color: root.outline }
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 6
                ActionButton {
                    text: "Overview"; checkable: true; checked: root.currentPage === 0
                    Layout.fillWidth: true
                    onClicked: root.currentPage = 0
                }
                ActionButton {
                    text: "Processes"; checkable: true; checked: root.currentPage === 1
                    Layout.fillWidth: true
                    onClicked: root.currentPage = 1
                }
                ActionButton {
                    text: "Performance"; checkable: true; checked: root.currentPage === 2
                    Layout.fillWidth: true
                    onClicked: root.currentPage = 2
                }
                ActionButton {
                    text: "Settings"; checkable: true; checked: root.currentPage === 3
                    Layout.fillWidth: true
                    onClicked: root.currentPage = 3
                }
                ActionButton {
                    text: "Applications"; checkable: true; checked: root.currentPage === 4
                    Layout.fillWidth: true
                    onClicked: root.currentPage = 4
                }
                Item { Layout.fillHeight: true }
            }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.currentPage
            Layout.margins: 20
            ScrollView {
                id: overviewScroll
                objectName: "overviewScroll"
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: overviewScroll.availableWidth
                    spacing: 18
                    Heading { text: "Overview"; level: 1; color: "#edf0f3" }
                    Label {
                        visible: root.backend.systemQuality !== "valid"
                        text: "System metrics: " + root.qualityLabel(root.backend.systemQuality)
                            + (root.backend.systemAgeMs >= 0 ? ". Showing last received values." : ".")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true
                        Repeater {
                            model: [
                                {title: "CPU busy", key: "cpuBusyPercent", scale: 1, suffix: " %", primary: true, tint: "#8bbce8"},
                                {title: "RAM used", key: "usedBytes", scale: 1073741824, suffix: " GiB", primary: true, tint: "#8bd1bc"},
                                {title: "CPU I/O wait", key: "cpuIowaitPercent", scale: 1, suffix: " %"},
                                {title: "RAM available", key: "availableBytes", scale: 1073741824, suffix: " GiB"},
                                {title: "Swap used", key: "swapUsedBytes", scale: 1073741824, suffix: " GiB"},
                                {title: "Uptime", key: "uptimeSeconds", scale: 3600, suffix: " h"}
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: modelData.primary ? 130 : 100
                                Layout.minimumHeight: implicitHeight
                                color: root.surface
                                radius: 7
                                border.color: root.outline
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 6
                                    Label { text: modelData.title; color: root.mutedText }
                                    Label {
                                        text: root.metric(modelData.key, modelData.scale, modelData.suffix)
                                        font.pixelSize: modelData.primary ? 34 : 24
                                        color: modelData.primary ? modelData.tint : "#edf0f3"
                                        Accessible.name: modelData.title + " " + text
                                    }
                                    Rectangle {
                                        visible: !!modelData.primary
                                        Layout.fillWidth: true
                                        implicitHeight: 3
                                        radius: 1.5
                                        color: "#2a3139"
                                        Rectangle {
                                            height: parent.height
                                            radius: parent.radius
                                            color: modelData.tint || root.accent
                                            width: {
                                                const reading = root.backend.system[modelData.key];
                                                if (!reading || reading.quality !== "valid" || reading.value === undefined
                                                    || reading.value === null || !isFinite(Number(reading.value))) return 0;
                                                if (modelData.key === "cpuBusyPercent") return parent.width * Math.min(1, Math.max(0, reading.value / 100));
                                                const total = root.backend.system.totalBytes;
                                                return total && total.quality === "valid" && total.value > 0
                                                    ? parent.width * Math.min(1, Math.max(0, reading.value / total.value)) : 0;
                                            }
                                        }
                                    }
                                    Label { text: root.fieldQuality(modelData.key); color: root.mutedText; font.pixelSize: 11 }
                                }
                            }
                        }
                    }
                    Label { text: "Load average (1 / 5 / 15 min): " + root.metric("load1", 1, "") + " / " + root.metric("load5", 1, "") + " / " + root.metric("load15", 1, "") }
                    CoreCpuSummary {
                        Layout.fillWidth: true
                        backend: root.backend
                    }
                    IoSummary {
                        objectName: "ioSummary"
                        Layout.fillWidth: true
                        backend: root.backend
                    }
                    Item { Layout.fillHeight: true }
                }
            }
            Pane {
                padding: 0
                background: Item {}
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 0
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 20
                        Heading { text: "Processes"; level: 1; color: "#edf0f3" }
                        TextField {
                            id: processSearch
                            objectName: "processSearch"
                            Layout.fillWidth: true
                            implicitHeight: 36
                            leftPadding: 12
                            background: Rectangle {
                                color: "#121519"
                                radius: 5
                                border.color: processSearch.activeFocus ? root.accent : root.outline
                            }
                            placeholderText: "Search name or PID"
                            Accessible.name: "Search processes by name or PID"
                            maximumLength: 256
                            text: root.backend.processView.query
                            onTextEdited: root.backend.processView.query = text
                        }
                    }
                    Label {
                        visible: root.backend.processQuality !== "valid" || root.backend.processes.count === 0
                        text: root.backend.processQuality !== "valid"
                            ? "Process metrics: " + root.qualityLabel(root.backend.processQuality)
                                + (root.backend.processAgeMs >= 0 ? ". Showing last received rows." : ".")
                            : "No visible processes."
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: root.backend.processView.count + " shown / " + root.backend.processes.count + " visible"
                                + (root.backend.processView.selectedRow >= 0 ? " · row selected" : "")
                        }
                        ActionButton {
                            objectName: "previousProcess"
                            text: "Previous process"
                            Accessible.name: text
                            enabled: root.backend.processView.selectedRow > 0
                            onClicked: {
                                root.backend.processView.selectRow(root.backend.processView.selectedRow - 1);
                                if (root.backend.processView.selectedRow >= 0) table.positionViewAtRow(root.backend.processView.selectedRow, TableView.Contain);
                            }
                        }
                        ActionButton {
                            objectName: "nextProcess"
                            text: "Next process"
                            Accessible.name: text
                            enabled: root.backend.processView.count > 0
                                && root.backend.processView.selectedRow < root.backend.processView.count - 1
                            onClicked: {
                                root.backend.processView.selectRow(root.backend.processView.selectedRow + 1);
                                if (root.backend.processView.selectedRow >= 0) table.positionViewAtRow(root.backend.processView.selectedRow, TableView.Contain);
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ActionButton {
                            objectName: "endProcess"
                            text: "End Process…"
                            enabled: root.processActions && !root.processActions.busy && !root.processActions.awaitingConfirmation
                                && root.backend.processView.selectedIdentity.length > 0
                            onClicked: root.processActions.prepare(root.backend.processView.selectedDetails.pid, false)
                        }
                        ActionButton {
                            objectName: "forceKillProcess"
                            text: "Force Kill…"
                            enabled: root.processActions && !root.processActions.busy && !root.processActions.awaitingConfirmation
                                && root.backend.processView.selectedIdentity.length > 0
                            onClicked: root.processActions.prepare(root.backend.processView.selectedDetails.pid, true)
                        }
                        ActionButton {
                            objectName: "endApplicationFromProcess"
                            text: "End Application…"
                            enabled: root.processActions && !root.processActions.busy && !root.processActions.awaitingConfirmation
                                && root.backend.processView.selectedIdentity.length > 0
                            onClicked: root.processActions.prepareApplication(root.backend.processView.selectedDetails.pid)
                        }
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            objectName: "cancelPendingProcessAction"
                            text: "Cancel pending"
                            visible: root.processActions && root.processActions.busy && root.processActions.cancellable
                            onClicked: root.processActions.cancel()
                        }
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(96, processStatusText.implicitHeight + 8)
                        clip: true
                        contentWidth: availableWidth
                        TextArea {
                        id: processStatusText
                        objectName: "processActionStatus"
                        width: parent.width
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        textFormat: Text.PlainText
                        color: root.mutedText
                        font.pixelSize: 12
                        background: Item {}
                        text: root.processActions && root.processActions.message.length > 0 ? root.processActions.message
                            : "Actions support verified desktop applications in dedicated scopes."
                        Accessible.name: "Process action status"
                        }
                    }
                    HorizontalHeaderView {
                        id: tableHeader
                        syncView: table
                        delegate: ActionButton {
                            required property var model
                            required property int column
                            implicitHeight: 30
                            text: model.display + (root.sortColumn === column ? (root.sortDescending ? " ↓" : " ↑") : "")
                            onClicked: {
                                root.sortDescending = root.sortColumn === column ? !root.sortDescending : column === 2 || column === 3;
                                root.sortColumn = column;
                                root.backend.processView.sortBy(column, root.sortDescending);
                            }
                        }
                        Layout.fillWidth: true
                        clip: true
                    }
                    TableView {
                        id: table
                        objectName: "processTable"
                        Layout.fillHeight: true
                        Layout.fillWidth: true
                        clip: true
                        model: root.backend.processView
                        activeFocusOnTab: true
                        Accessible.name: "Process table"
                        Accessible.focusable: true
                        Accessible.description: "Use Up and Down to select a process, then Tab to read its details. Previous process and Next process buttons provide the same selection."
                        KeyNavigation.priority: KeyNavigation.BeforeItem
                        KeyNavigation.tab: processDetails.visible ? processDetails : processSearch
                        Keys.onDownPressed: {
                            root.backend.processView.selectRow(Math.min(root.backend.processView.count - 1, root.backend.processView.selectedRow + 1));
                            if (root.backend.processView.selectedRow >= 0) table.positionViewAtRow(root.backend.processView.selectedRow, TableView.Contain);
                        }
                        Keys.onUpPressed: {
                            root.backend.processView.selectRow(Math.max(0, root.backend.processView.selectedRow - 1));
                            if (root.backend.processView.selectedRow >= 0) table.positionViewAtRow(root.backend.processView.selectedRow, TableView.Contain);
                        }
                        Keys.onEscapePressed: root.backend.processView.clearSelection()
                        reuseItems: true
                        columnSpacing: 0
                        rowSpacing: 1
                        onWidthChanged: forceLayout()
                        columnWidthProvider: function(column) {
                            const fixed = [70, 0, 80, 95, 65, 85];
                            return column === 1 ? Math.max(170, table.width - 400) : fixed[column];
                        }
                        delegate: Rectangle {
                            id: cell
                            required property var display
                            required property int row
                            required property int column
                            implicitWidth: 100
                            implicitHeight: 28
                            color: root.backend.processView.selectedRow === row ? "#243e54" : row % 2 === 0 ? "#15191e" : "#101419"
                            TapHandler {
                                onTapped: {
                                    root.backend.processView.selectRow(cell.row);
                                    table.forceActiveFocus();
                                }
                            }
                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: cell.column === 1 || cell.column === 4 ? Text.AlignLeft : Text.AlignRight
                                text: cell.display
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: "#edf0f3"
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                        ScrollBar.horizontal: ScrollBar {}
                    }
                    Frame {
                        padding: 8
                        background: Rectangle { color: root.surface; radius: 7; border.color: root.outline }
                        id: detailsPanel
                        objectName: "selectedProcessDetails"
                        property var details: root.backend.processView.selectedDetails
                        visible: root.backend.processView.selectedIdentity.length > 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: 104
                        Layout.minimumHeight: 88
                        Layout.maximumHeight: 104
                        Connections {
                            target: root.backend.processView
                            function onSelectedDetailsChanged() {
                                if (root.currentPage === 1 && processDetails.activeFocus
                                    && root.backend.processView.selectedIdentity.length === 0) table.forceActiveFocus();
                            }
                        }
                        contentItem: ScrollView {
                            id: detailsScroll
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: processDetails
                                objectName: "processDetailsText"
                                width: detailsScroll.availableWidth
                                readOnly: true
                                selectByMouse: true
                                activeFocusOnTab: true
                                textFormat: TextEdit.PlainText
                                background: Item {}
                                wrapMode: TextEdit.WrapAnywhere
                                Accessible.name: "Selected process details"
                                Accessible.readOnly: true
                                Accessible.multiLine: true
                                Accessible.focusable: true
                                Accessible.description: "Read-only process observation. Tab returns to search, Shift+Tab returns to the process table, Escape clears the selection."
                                KeyNavigation.priority: KeyNavigation.BeforeItem
                                KeyNavigation.tab: processSearch
                                KeyNavigation.backtab: table
                                Keys.onEscapePressed: {
                                    root.backend.processView.clearSelection();
                                    table.forceActiveFocus();
                                }
                                text: !detailsPanel.visible ? "" : detailsPanel.details.name + " · PID " + detailsPanel.details.pid
                                    + " · " + root.qualityLabel(root.backend.processQuality)
                                    + "\nCPU " + root.detailMetric(detailsPanel.details.cpuPercent, detailsPanel.details.cpuQuality, 1, " %")
                                    + " · RSS " + root.detailMetric(detailsPanel.details.rssBytes, detailsPanel.details.rssQuality, 1048576, " MiB")
                                    + "\nParent PID " + detailsPanel.details.parentPid + " · UID " + detailsPanel.details.uid
                                    + " · threads " + detailsPanel.details.threads + " · state " + detailsPanel.details.state
                                    + "\nStart time: " + detailsPanel.details.startTicks + " ticks since boot"
                                    + "\nScope naming hint: " + (detailsPanel.details.scopeUnit || "—")
                                    + " · " + root.qualityLabel(detailsPanel.details.scopeQuality || "unsupported")
                            }
                        }
                    }
                }
            }
            ScrollView {
                id: performanceScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: performanceScroll.availableWidth
                    spacing: 16
                    Heading { text: "Performance"; level: 1; color: "#edf0f3" }
                    Frame {
                        padding: 16
                        background: Rectangle { color: root.surface; radius: 7; border.color: root.outline }
                        objectName: "gpuSection"
                        Layout.fillWidth: true
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8
                            Label {
                                objectName: "gpuStatusText"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: !root.backend.gpuEnabled ? "GPU monitoring: disabled"
                                    : "NVIDIA GPU: " + root.qualityLabel(root.backend.gpuQuality)
                                      + " · data age " + (root.backend.gpuAgeMs < 0 ? "—" : root.backend.gpuAgeMs + " ms")
                                      + " · issues " + root.backend.gpuIssueCount
                                Accessible.name: text
                            }
                            Label {
                                visible: root.backend.gpuEnabled && root.backend.gpuDevices.length === 0
                                text: "No GPU readings."
                            }
                            Label {
                                objectName: "gpuMessageText"
                                visible: root.backend.gpuMessage.length > 0
                                text: root.backend.gpuMessage
                                textFormat: Text.PlainText
                                wrapMode: Text.WrapAnywhere
                                Layout.fillWidth: true
                                Accessible.name: text
                            }
                            Repeater {
                                model: performanceScroll.visible ? root.backend.gpuDevices : []
                                delegate: ColumnLayout {
                                    required property var modelData
                                    required property int index
                                    objectName: "gpuDeviceRow_" + index
                                    Layout.fillWidth: true
                                    Label {
                                        text: modelData.name.value || "Unnamed GPU"
                                        textFormat: Text.PlainText
                                        wrapMode: Text.WrapAnywhere
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        text: "GPU utilization: " + root.detailMetric(modelData.utilizationPercent.value,
                                            modelData.utilizationPercent.quality, 1, " %")
                                        textFormat: Text.PlainText
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        text: "VRAM used: " + root.detailMetric(modelData.usedBytes.value, modelData.usedBytes.quality, 1048576, " MiB")
                                            + " · total: " + root.detailMetric(modelData.totalBytes.value, modelData.totalBytes.quality, 1048576, " MiB")
                                            + " · free: " + root.detailMetric(modelData.freeBytes.value, modelData.freeBytes.quality, 1048576, " MiB")
                                        textFormat: Text.PlainText
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        objectName: "gpuSensors_" + index
                                        text: "Temperature: " + root.detailMetric(modelData.temperatureCelsius.value,
                                            modelData.temperatureCelsius.quality, 1, " °C")
                                            + " · power draw: " + root.detailMetric(modelData.powerWatts.value,
                                            modelData.powerWatts.quality, 1, " W")
                                        textFormat: Text.PlainText
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                        Accessible.name: text
                                    }
                                }
                            }
                            Label {
                                visible: root.backend.gpuEnabled
                                text: root.backend.gpuSamplingWindow + " · polling: " + root.backend.configuration.gpuMs + " ms"
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                    Label {
                        text: "Recent history · " + root.backend.history.length + " samples · gaps indicate unavailable data"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    HistoryPlot {
                        objectName: "cpuHistoryPlot"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 220
                        points: root.backend.history
                        metric: "cpu"
                        title: "CPU busy (%)"
                        upperBound: 100
                    }
                    HistoryPlot {
                        Layout.fillWidth: true
                        Layout.minimumHeight: 220
                        points: root.backend.history
                        metric: "availableGiB"
                        title: "Available RAM (GiB)"
                        upperBound: {
                            let maximum = 1;
                            for (let i = 0; i < points.length; ++i) {
                                const value = points[i].availableGiB;
                                if (typeof value === "number" && isFinite(value)) maximum = Math.max(maximum, value);
                            }
                            return Math.ceil(maximum);
                        }
                    }
                }
            }
            ScrollView {
                id: settingsScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 16
                    Heading { text: "Settings"; level: 1; color: "#edf0f3" }
                    Label { text: "Active sampling configuration"; font.bold: true }
                    Label {
                        text: "Change the sampling preset while the monitor runs. Existing history and counter baselines are preserved."
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Label { text: "Refresh mode" }
                    ComboBox {
                        id: refreshModeSelector
                        objectName: "refreshModeSelector"
                        Layout.fillWidth: true
                        model: ["Fast", "Balanced", "Low Power"]
                        currentIndex: root.backend.refreshMode === "fast" ? 0
                            : root.backend.refreshMode === "balanced" ? 1 : root.backend.refreshMode === "low-power" ? 2 : -1
                        displayText: currentIndex < 0 ? "Custom intervals" : currentText
                        Accessible.name: "Refresh mode"
                        Accessible.description: "Choose Fast, Balanced, or Low Power. Changes apply without restarting."
                        onActivated: root.backend.setRefreshMode(["fast", "balanced", "low-power"][currentIndex])
                    }
                    Label { text: "System: " + root.backend.configuration.systemMs + " ms" }
                    Label { text: "Processes: " + root.backend.configuration.processMs + " ms" }
                    Label { text: "Network and disk I/O: " + root.backend.configuration.ioMs + " ms" }
                    Label { text: "Selected filesystem capacity: " + root.backend.configuration.capacityMs / 1000 + " s" }
                    Label { text: "GPU: " + (root.backend.gpuEnabled ? root.backend.configuration.gpuMs + " ms" : "disabled") }
                    Label { text: "History limit: " + root.backend.configuration.historyCapacity + " samples" }
                    Label { text: "Application scope observations: " + (root.backend.configuration.scopeMetadata ? "enabled" : "disabled") }
                    Label { text: "Metric definitions"; font.bold: true }
                    Label { text: "Process CPU 100% means one logical core." }
                    Label { text: "RAM availability uses the kernel estimate. Process RSS is approximate; shared pages can overlap."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: "Data ages refer to the last successful sample. Missing samples appear as gaps in charts."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
            }
            Pane {
                padding: 0
                background: Item {}
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 0
                    spacing: 12
                    Heading { text: "Applications"; level: 1; color: "#edf0f3" }
                    Label {
                        text: "Observed scope naming candidates. Application identity and complete membership are unverified; RSS sums may overlap shared pages."
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: "Scope data: " + root.qualityLabel(root.backend.applications.quality)
                            + " · age " + (root.backend.applications.dataAgeMs < 0 ? "—" : root.backend.applications.dataAgeMs + " ms")
                            + " · " + root.backend.applications.unclassified + " unclassified · " + root.backend.applications.unavailable + " unavailable"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: root.backend.applications.count === 0 || (root.backend.applications.quality !== "valid" && root.backend.applications.quality !== "partial")
                        text: root.backend.applications.quality === "valid"
                            ? "No application-scope candidates in the visible process set."
                            : "Scope observations: " + root.qualityLabel(root.backend.applications.quality)
                                + (root.backend.applications.dataAgeMs >= 0 ? ". Showing last received rows." : ".")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            text: root.selectedApplicationUnit.length > 0 ? root.selectedApplicationUnit : "Select an observed application scope"
                        }
                        ActionButton {
                            objectName: "endApplication"
                            text: "End Application…"
                            enabled: root.processActions && !root.processActions.busy && !root.processActions.awaitingConfirmation
                                && root.backend.applications.count > 0
                                && root.backend.applications.seedPidForUnit(root.selectedApplicationUnit).length > 0
                            onClicked: root.processActions.prepareApplication(root.backend.applications.seedPidForUnit(root.selectedApplicationUnit))
                        }
                        ActionButton {
                            text: "Cancel pending"
                            visible: root.processActions && root.processActions.busy && root.processActions.cancellable
                            onClicked: root.processActions.cancel()
                        }
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(96, applicationStatusText.implicitHeight + 8)
                        contentWidth: availableWidth
                        clip: true
                        TextArea {
                            id: applicationStatusText
                            width: parent.width
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            textFormat: TextEdit.PlainText
                            color: root.mutedText
                            font.pixelSize: 12
                            background: Item {}
                            text: root.processActions ? root.processActions.message : ""
                            Accessible.name: "Application action results"
                        }
                    }
                    HorizontalHeaderView {
                        syncView: applicationsTable
                        Layout.fillWidth: true
                        clip: true
                        delegate: Rectangle {
                            required property var model
                            implicitHeight: 30
                            color: "#1b2026"
                            Text {
                                anchors.fill: parent
                                anchors.margins: 8
                                text: model.display
                                color: root.mutedText
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                        }
                    }
                    TableView {
                        id: applicationsTable
                        objectName: "applicationTable"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        reuseItems: true
                        model: root.backend.applications
                        activeFocusOnTab: true
                        Accessible.name: "Application scope table"
                        Accessible.description: "Use Up and Down to select a scope, then End Application to verify a fresh fixed member set."
                        Keys.onDownPressed: root.selectedApplicationUnit = root.backend.applications.unitAtRow(Math.min(root.backend.applications.count - 1,
                            root.backend.applications.rowForUnit(root.selectedApplicationUnit) + 1))
                        Keys.onUpPressed: root.selectedApplicationUnit = root.backend.applications.unitAtRow(Math.max(0,
                            root.backend.applications.rowForUnit(root.selectedApplicationUnit) - 1))
                        Keys.onEscapePressed: root.selectedApplicationUnit = ""
                        onWidthChanged: forceLayout()
                        columnWidthProvider: function(column) {
                            return column === 0 ? Math.max(280, applicationsTable.width - 390) : [0, 100, 130, 160][column];
                        }
                        delegate: Rectangle {
                            id: applicationCell
                            required property var display
                            required property int row
                            implicitWidth: 100
                            implicitHeight: 28
                            color: root.backend.applications.unitAtRow(row) === root.selectedApplicationUnit ? "#243e54"
                                : row % 2 === 0 ? "#15191e" : "#101419"
                            TapHandler {
                                onTapped: {
                                    root.selectedApplicationUnit = root.backend.applications.unitAtRow(applicationCell.row);
                                    applicationsTable.forceActiveFocus();
                                }
                            }
                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                verticalAlignment: Text.AlignVCenter
                                text: applicationCell.display
                                textFormat: Text.PlainText
                                color: "#edf0f3"
                                elide: Text.ElideRight
                            }
                        }
                        ScrollBar.horizontal: ScrollBar {}
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
            }
        }
    }
}
