import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// archive — follow curated LEZ channels, preserve collections to Storage.
// Dark theme matching radio/keeper/stash. Sandbox rules (qml-sandbox-restrictions):
// no network/FileDialog/Qt.openUrlExternally here — all IO lives in the C++ backend;
// provenance links are copy-to-clipboard. Inside layouts use implicitHeight, never height.
Item {
    id: root
    width: 560; height: 720

    // ── Dark palette (radio/keeper) ──────────────────────────────────────────
    readonly property color bgPrimary:     "#171717"
    readonly property color bgSecondary:   "#262626"
    readonly property color bgActive:      "#332A27"
    readonly property color textPrimary:   "#FFFFFF"
    readonly property color textSecondary: "#A4A4A4"
    readonly property color textMuted:     "#5D5D5D"
    readonly property color accentOrange:  "#FF5000"
    readonly property color successGreen:  "#22C55E"
    readonly property color warningYellow: "#F59E0B"
    readonly property color errorRed:      "#FB3748"
    readonly property color borderColor:   "#383838"

    // ── Backend bridge (QRO replica) ─────────────────────────────────────────
    readonly property var backend: logos.module("archive")
    readonly property string gatewayState: backend ? backend.gatewayState : "offline"
    readonly property int    syncLag:      backend ? backend.syncLagBlocks : 0
    readonly property string storageState: backend ? backend.storageState : "offline"
    readonly property string preserveMode: backend ? backend.preserveMode : "delegate"
    readonly property string lastError:    backend ? backend.lastError : ""

    property var channels: []
    property var collections: []
    property var summary: ({})
    property bool settingsOpen: false

    readonly property string channelsRaw:    backend ? backend.channelsJson : "[]"
    readonly property string collectionsRaw: backend ? backend.collectionsJson : "[]"
    readonly property string summaryRaw:     backend ? backend.summaryJson : "{}"
    onChannelsRawChanged:    channels = safeParse(channelsRaw, [])
    onCollectionsRawChanged: collections = safeParse(collectionsRaw, [])
    onSummaryRawChanged:     summary = safeParse(summaryRaw, {})
    onLastErrorChanged:      if (lastError) logEvent("error", lastError)
    onGatewayStateChanged:   logEvent("gateway", "Gateway " + gatewayState
                                      + (syncLag > 0 ? " · lag " + syncLag + " slots" : ""))

    function safeParse(raw, fallback) {
        try {
            var v = JSON.parse(raw)
            if (typeof v === "string") v = JSON.parse(v)   // defensive vs double-encoding
            return v === null ? fallback : v
        } catch (e) { return fallback }
    }

    // SLOT call → parsed {ok,...} result into cb; failures land in the activity log
    function call(method, args, cb) {
        if (!backend) { logEvent("error", "No backend — module not loaded"); return }
        var pending = backend[method].apply(backend, args)
        logos.watch(pending, function(ret) {
            var r = safeParse(ret, null)
            if (r && r.ok === false) logEvent("error", method + ": " + r.error)
            if (cb) cb(r)
        }, function(_e) { logEvent("error", method + ": call failed") })
    }

    function gb(bytes) { return (bytes / 1e9).toFixed(bytes > 0 && bytes < 1e8 ? 2 : 1) }
    function shortId(s) { return s ? s.substring(0, 8) + "…" : "" }
    function explorerUrl(tx) {
        return "https://testnet.blockchain.logos.co/web/explorer/transactions/" + tx
    }
    function copyText(t) { clipHelper.text = t; clipHelper.selectAll(); clipHelper.copy(); clipHelper.text = "" }
    TextEdit { id: clipHelper; visible: false }

    // ── Activity log ─────────────────────────────────────────────────────────
    ListModel { id: activityModel }
    function logEvent(kind, text) {
        activityModel.insert(0, { kind: kind, text: text,
                                  ts: Qt.formatDateTime(new Date(), "hh:mm:ss") })
        if (activityModel.count > 200) activityModel.remove(200, activityModel.count - 200)
    }

    function stateColor(s) {
        return s === "ready" ? successGreen
             : s === "degraded" ? warningYellow : errorRed
    }
    function mirrorColor(s) {
        return s === "mirrored" ? successGreen
             : s === "mirroring" ? warningYellow
             : s === "error" ? errorRed : textMuted
    }

    Rectangle { anchors.fill: parent; color: root.bgPrimary }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ── Header: title + pills + cogwheel ─────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "Archive"
                color: root.textPrimary
                font.pixelSize: 20; font.bold: true
            }
            Item { Layout.fillWidth: true }

            // Gateway pill — surfaces LEZ#519 lag directly
            Rectangle {
                implicitWidth: gwRow.implicitWidth + 16; implicitHeight: 24; radius: 12
                color: root.bgSecondary; border.color: root.borderColor
                RowLayout {
                    id: gwRow; anchors.centerIn: parent; spacing: 5
                    Rectangle { width: 7; height: 7; radius: 3.5; color: root.stateColor(root.gatewayState) }
                    Label {
                        text: "Gateway " + root.gatewayState
                              + (root.gatewayState === "degraded" ? " · lag " + root.syncLag : "")
                        color: root.textSecondary; font.pixelSize: 11
                    }
                }
            }
            Rectangle {
                implicitWidth: stRow.implicitWidth + 16; implicitHeight: 24; radius: 12
                color: root.bgSecondary; border.color: root.borderColor
                RowLayout {
                    id: stRow; anchors.centerIn: parent; spacing: 5
                    Rectangle { width: 7; height: 7; radius: 3.5; color: root.stateColor(root.storageState) }
                    Label { text: "Storage " + root.storageState; color: root.textSecondary; font.pixelSize: 11 }
                }
            }
            Rectangle {
                implicitWidth: modeLbl.implicitWidth + 16; implicitHeight: 24; radius: 12
                color: root.bgActive; border.color: root.borderColor
                Label {
                    id: modeLbl; anchors.centerIn: parent
                    text: root.preserveMode === "local" ? "Local" : "Delegate"
                    color: root.accentOrange; font.pixelSize: 11; font.bold: true
                }
            }
            ToolButton {
                text: "⚙"
                onClicked: root.settingsOpen = !root.settingsOpen
                contentItem: Label {
                    text: parent.text; color: root.settingsOpen ? root.accentOrange : root.textSecondary
                    font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle { color: "transparent" }
            }
        }

        // ── Settings (cogwheel) ──────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.settingsOpen ? settingsCol.implicitHeight + 24 : 0
            visible: implicitHeight > 0
            radius: 8; color: root.bgSecondary; border.color: root.borderColor
            clip: true
            Behavior on implicitHeight { NumberAnimation { duration: 120 } }

            ColumnLayout {
                id: settingsCol
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                spacing: 8
                Label { text: "Preserve mode"; color: root.textPrimary; font.bold: true; font.pixelSize: 13 }
                RadioButton {
                    text: "Delegate — the gateway pins for you (zero-infra)"
                    checked: root.preserveMode === "delegate"
                    onClicked: root.call("choosePreserveMode", ["delegate"],
                                         function(r) { if (r && r.ok) root.logEvent("config", "Mode → delegate") })
                    contentItem: Label {
                        text: parent.text; color: root.textSecondary; font.pixelSize: 12
                        leftPadding: parent.indicator.width + 6; verticalAlignment: Text.AlignVCenter
                    }
                }
                RadioButton {
                    text: "Local — replicate to your own Storage node"
                    checked: root.preserveMode === "local"
                    onClicked: root.call("choosePreserveMode", ["local"],
                                         function(r) { if (r && r.ok) root.logEvent("config", "Mode → local") })
                    contentItem: Label {
                        text: parent.text; color: root.textSecondary; font.pixelSize: 12
                        leftPadding: parent.indicator.width + 6; verticalAlignment: Text.AlignVCenter
                    }
                }
                Label { text: "Gateway"; color: root.textPrimary; font.bold: true; font.pixelSize: 13 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: nodeUrlField
                        Layout.fillWidth: true
                        placeholderText: "node URL — http://gateway:8080"
                        color: root.textPrimary; placeholderTextColor: root.textMuted
                        background: Rectangle { color: root.bgPrimary; radius: 4; border.color: root.borderColor }
                    }
                    TextField {
                        id: storageUrlField
                        Layout.fillWidth: true
                        placeholderText: "storage URL — http://gateway:5001"
                        color: root.textPrimary; placeholderTextColor: root.textMuted
                        background: Rectangle { color: root.bgPrimary; radius: 4; border.color: root.borderColor }
                    }
                    Button {
                        text: "Apply"
                        enabled: nodeUrlField.text.length > 0
                        onClicked: root.call("setGateways",
                            [JSON.stringify([{ nodeUrl: nodeUrlField.text, storageUrl: storageUrlField.text }])],
                            function(r) { if (r && r.ok) root.logEvent("config", "Gateway set: " + nodeUrlField.text) })
                    }
                }
            }
        }

        // ── Summary counter — the campaign hook ──────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 46; radius: 8
            color: root.bgActive; border.color: root.borderColor
            RowLayout {
                anchors.fill: parent; anchors.margins: 12; spacing: 6
                Label {
                    text: "You're preserving "
                          + (root.summary.mirrored || 0) + " collection"
                          + ((root.summary.mirrored || 0) === 1 ? "" : "s")
                          + " · " + root.gb(root.summary.usedBytes || 0) + " GB"
                    color: root.textPrimary; font.pixelSize: 14; font.bold: true
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: "following " + (root.summary.following || 0)
                          + " · " + (root.summary.collections || 0) + " collections"
                    color: root.textSecondary; font.pixelSize: 11
                }
            }
        }

        // ── Tabs ─────────────────────────────────────────────────────────────
        TabBar {
            id: tabs
            Layout.fillWidth: true
            background: Rectangle { color: root.bgPrimary }
            Repeater {
                model: ["Channels", "Collections", "Activity"]
                TabButton {
                    text: modelData
                    contentItem: Label {
                        text: parent.text
                        color: tabs.currentIndex === index ? root.accentOrange : root.textSecondary
                        font.pixelSize: 13; font.bold: tabs.currentIndex === index
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: root.bgPrimary
                        Rectangle {
                            anchors.bottom: parent.bottom; width: parent.width; height: 2
                            color: tabs.currentIndex === index ? root.accentOrange : "transparent"
                        }
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ── Channels tab ─────────────────────────────────────────────────
            ColumnLayout {
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: followField
                        Layout.fillWidth: true
                        placeholderText: "channel id, id@startSlot, or explorer URL"
                        color: root.textPrimary; placeholderTextColor: root.textMuted
                        background: Rectangle { color: root.bgSecondary; radius: 4; border.color: root.borderColor }
                        onAccepted: followBtn.clicked()
                    }
                    Button {
                        id: followBtn
                        text: "Follow"
                        enabled: followField.text.length > 0
                        onClicked: root.call("followChannel", [followField.text], function(r) {
                            if (r && r.ok) {
                                root.logEvent("follow", "Following " + root.shortId(r.channelId))
                                followField.text = ""
                            }
                        })
                    }
                }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    model: root.channels
                    clip: true
                    spacing: 6
                    delegate: Rectangle {
                        width: ListView.view.width
                        implicitHeight: chRow.implicitHeight + 20
                        radius: 8; color: root.bgSecondary; border.color: root.borderColor
                        RowLayout {
                            id: chRow
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 12 }
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label { text: modelData.name; color: root.textPrimary; font.pixelSize: 13; font.bold: true }
                                Label {
                                    text: (modelData.curator ? "curator " + root.shortId(modelData.curator) + " · " : "")
                                          + modelData.collections + " collections"
                                    color: root.textSecondary; font.pixelSize: 11
                                }
                            }
                            Rectangle {
                                implicitWidth: syncLbl.implicitWidth + 12; implicitHeight: 20; radius: 10
                                color: "transparent"; border.color: modelData.synced ? root.successGreen : root.warningYellow
                                Label {
                                    id: syncLbl; anchors.centerIn: parent
                                    text: modelData.synced ? "synced" : "syncing…"
                                    color: modelData.synced ? root.successGreen : root.warningYellow
                                    font.pixelSize: 10
                                }
                            }
                            ToolButton {
                                text: "↻"
                                onClicked: root.call("refreshChannel", [modelData.channelId], function(r) {
                                    if (r && r.ok) root.logEvent("refresh", "Refreshing " + root.shortId(modelData.channelId))
                                })
                                contentItem: Label { text: parent.text; color: root.textSecondary; font.pixelSize: 14 }
                                background: Rectangle { color: "transparent" }
                            }
                            ToolButton {
                                text: "✕"
                                onClicked: root.call("unfollowChannel", [modelData.channelId], function(r) {
                                    if (r && r.ok) root.logEvent("follow", "Unfollowed " + root.shortId(modelData.channelId))
                                })
                                contentItem: Label { text: parent.text; color: root.errorRed; font.pixelSize: 12 }
                                background: Rectangle { color: "transparent" }
                            }
                        }
                    }
                    Label {
                        visible: root.channels.length === 0
                        anchors.centerIn: parent
                        text: "Follow a curated channel to see its collections.\nReads need a synced gateway (LEZ#519)."
                        color: root.textMuted; font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ── Collections tab ──────────────────────────────────────────────
            ColumnLayout {
                spacing: 8
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    model: root.collections
                    clip: true
                    spacing: 6
                    delegate: Rectangle {
                        width: ListView.view.width
                        implicitHeight: colCol.implicitHeight + 20
                        radius: 8; color: root.bgSecondary; border.color: root.borderColor
                        ColumnLayout {
                            id: colCol
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 12 }
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.title
                                    color: root.textPrimary; font.pixelSize: 13; font.bold: true
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    implicitWidth: stLbl.implicitWidth + 12; implicitHeight: 20; radius: 10
                                    color: "transparent"; border.color: root.mirrorColor(modelData.state)
                                    Label {
                                        id: stLbl; anchors.centerIn: parent
                                        text: modelData.state
                                              + (modelData.state === "mirroring" && modelData.progressBlocks > 0
                                                 ? " · " + modelData.progressBlocks : "")
                                        color: root.mirrorColor(modelData.state); font.pixelSize: 10
                                    }
                                }
                                Button {
                                    text: modelData.state === "mirrored" ? "Unpreserve" : "Preserve"
                                    enabled: modelData.state !== "mirroring"
                                    onClicked: {
                                        if (modelData.state === "mirrored")
                                            root.call("unmirrorCollection", [modelData.id], function(r) {
                                                if (r && r.ok) root.logEvent("mirror", "Unpreserving " + modelData.title)
                                            })
                                        else
                                            root.call("mirrorCollection", [modelData.id], function(r) {
                                                if (r && r.ok) root.logEvent("mirror",
                                                    "Preserving " + modelData.title + " (" + r.mode + ")")
                                            })
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    text: (modelData.sizeBytes > 0 ? root.gb(modelData.sizeBytes) + " GB · " : "")
                                          + (modelData.items > 0 ? modelData.items + " items · " : "")
                                          + "slot " + modelData.inscribedAt
                                    color: root.textSecondary; font.pixelSize: 11
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: "tx " + root.shortId(modelData.txHash)
                                    color: root.textMuted; font.pixelSize: 10
                                }
                                ToolButton {
                                    text: "copy link"
                                    onClicked: {
                                        root.copyText(root.explorerUrl(modelData.txHash))
                                        root.logEvent("copy", "Provenance link copied — " + root.shortId(modelData.txHash))
                                    }
                                    contentItem: Label { text: parent.text; color: root.accentOrange; font.pixelSize: 10 }
                                    background: Rectangle { color: "transparent" }
                                }
                            }
                        }
                    }
                    Label {
                        visible: root.collections.length === 0
                        anchors.centerIn: parent
                        text: "No collections yet — follow a channel first."
                        color: root.textMuted; font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ── Activity tab ─────────────────────────────────────────────────
            ListView {
                model: activityModel
                clip: true
                spacing: 2
                delegate: RowLayout {
                    width: ListView.view.width
                    spacing: 8
                    Label { text: model.ts; color: root.textMuted; font.pixelSize: 10 }
                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: model.kind === "error" ? root.errorRed
                             : model.kind === "mirror" ? root.successGreen
                             : model.kind === "gateway" ? root.warningYellow : root.textSecondary
                    }
                    Label {
                        Layout.fillWidth: true
                        text: model.text
                        color: model.kind === "error" ? root.errorRed : root.textSecondary
                        font.pixelSize: 11; elide: Text.ElideRight
                    }
                }
                Label {
                    visible: activityModel.count === 0
                    anchors.centerIn: parent
                    text: "Follows, preserves and gateway events appear here."
                    color: root.textMuted; font.pixelSize: 12
                }
            }
        }
    }
}
