import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// archive — follow curated LEZ channels, preserve items to Storage.
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

    // ── Backend bridge: logos.callModule("archive", …) + guarded polling ─────
    // (stash_ui/radio-main pattern — the core module runs in logos_host; the QML
    // bridge in the main process owns the capability tokens. callModule blocks the
    // JS thread, so every Timer-driven poll carries a reentrancy guard.)
    property string gatewayState: "offline"
    property int    syncLag: 0
    property string storageState: "offline"
    property string preserveMode: "local"
    property string activeGatewayUrl: ""
    // channels/items live in ListModels (not JS arrays): in-place role updates
    // keep delegate identity stable, so the viewport never jumps on poll/Preserve (#12)
    ListModel { id: channelsModel }
    ListModel { id: itemsModel }

    // every role always present with a stable type — ListModel locks roles on first
    // append, and absent-vs-undefined mismatches corrupt later setProperty calls
    function normalizeChannel(h) {
        return { channelId: h.channelId || "", name: h.name || "",
                 curator: h.curator || "", items: h.items || 0,
                 lastInscription: h.lastInscription || 0, synced: h.synced === true,
                 autoPreserve: h.autoPreserve === true,
                 progress: h.progress !== undefined ? h.progress : -1 }
    }
    function normalizeItem(c) {
        return { id: c.id || "", title: c.title || "", channelId: c.channelId || "",
                 cid: c.cid || "", sizeBytes: c.sizeBytes || 0, items: c.items || 0,
                 thumbnail: c.thumbnail || "", inscribedAt: c.inscribedAt || 0,
                 txHash: c.txHash || "", blockHash: c.blockHash || "",
                 txIndex: c.txIndex !== undefined ? c.txIndex : -1,
                 curator: c.curator || "", state: c.state || "available",
                 progressBlocks: c.progressBlocks || 0,
                 iaId: c.iaId || "", iaFile: c.iaFile || "" }
    }

    // diff rows (already normalized) into the model by key: changed roles updated
    // in place, new rows appended, vanished rows removed — never a full rebuild
    function syncListModel(model, rows, key) {
        var have = {}
        for (var i = 0; i < model.count; i++) have[model.get(i)[key]] = i
        var seen = {}
        for (var r = 0; r < rows.length; r++) {
            var row = rows[r]
            seen[row[key]] = true
            var idx = have[row[key]]
            if (idx === undefined) {
                model.append(row)
            } else {
                var cur = model.get(idx)
                for (var role in row)
                    if (cur[role] !== row[role]) model.setProperty(idx, role, row[role])
            }
        }
        for (var j = model.count - 1; j >= 0; j--)
            if (!seen[model.get(j)[key]]) model.remove(j)
    }
    property var summary: ({})
    property bool settingsOpen: false

    function safeParse(raw, fallback) {
        try {
            var v = JSON.parse(raw)
            if (typeof v === "string") v = JSON.parse(v)   // callModule double-encodes
            return v === null ? fallback : v
        } catch (e) { return fallback }
    }

    // sync call → parsed {ok,...}; failures land in the activity log.
    // callModule pumps the event loop while waiting — pollBusy blocks the Timer from
    // refreshing models mid-call (a model reset destroys delegates and their closures).
    function call(method, args, cb) {
        var wasBusy = root.pollBusy
        root.pollBusy = true
        var r = null
        try {
            r = safeParse(logos.callModule("archive", method, args || []), null)
        } catch (e) {
            logEvent("error", method + ": " + e)
        } finally {
            root.pollBusy = wasBusy
        }
        if (r === null) logEvent("error", method + ": no response from archive module")
        else if (r.ok === false) logEvent("error", method + ": " + r.error)
        if (cb) cb(r)
        if (r && r.ok) Qt.callLater(root.poll)   // snappy refresh after any action
        return r
    }

    // Edge-triggered logging — polls re-deliver unchanged values; the activity log
    // records transitions, the pills show state.
    property string _loggedGatewayState: ""
    property string _loggedStorageState: ""
    property string _loggedError: ""
    property var _prevItemStates: ({})   // id → state, for transition logging
    property var _prevChannelSynced: ({})      // channelId → synced flag

    property bool pollBusy: false
    function poll() {
        if (root.pollBusy) return   // re-entrant Timer fire from inside callModule — skip
        root.pollBusy = true
        // finally is load-bearing: one thrown callModule used to leave pollBusy stuck
        // true forever — pills/stats froze at defaults until restart
        try {
            var s = null
            try {
                s = safeParse(logos.callModule("archive", "getStatus", []), null)
            } catch (e) { s = null }
            if (s && s.ok) {
                root.gatewayState = s.gatewayState || "offline"
                root.syncLag = s.syncLagBlocks || 0
                root.storageState = s.storageState || "offline"
                root.preserveMode = s.preserveMode || "local"
                root.activeGatewayUrl = s.activeGatewayUrl || ""
                root.summary = s.summary || {}
                if (s.lastError && s.lastError !== root._loggedError) {
                    root._loggedError = s.lastError
                    logEvent("error", s.lastError)
                }
                if (root.gatewayState !== root._loggedGatewayState) {
                    root._loggedGatewayState = root.gatewayState
                    logEvent("gateway", "Gateway " + root.gatewayState
                             + (root.syncLag > 0 && root.gatewayState === "degraded"
                                ? " (lag " + root.syncLag + " slots)" : ""))
                }
                if (root.storageState !== root._loggedStorageState) {
                    root._loggedStorageState = root.storageState
                    logEvent(root.storageState === "ready" ? "mirror" : "gateway",
                             "Storage " + root.storageState)
                }
                try {
                    var ch = safeParse(logos.callModule("archive", "getChannels", []), null)
                    if (ch && ch.ok) {
                        var chNew = ch.channels || []
                        for (var hi = 0; hi < chNew.length; hi++) {
                            var h = chNew[hi]
                            var prevSync = root._prevChannelSynced[h.channelId]
                            if (prevSync === false && h.synced === true)
                                logEvent("mirror", "Channel " + h.name + " fully synced"
                                         + root.scanSummary(h.channelId))
                            root._prevChannelSynced[h.channelId] = h.synced
                        }
                        syncListModel(channelsModel, chNew.map(normalizeChannel), "channelId")
                    }
                } catch (e1) {}
                try {
                    var co = safeParse(logos.callModule("archive", "getItems", [""]), null)
                    if (co && co.ok) {
                        var coNew = co.items || []
                        // per-item preservation transitions → activity log
                        for (var ci = 0; ci < coNew.length; ci++) {
                            var c = coNew[ci]
                            var prev = root._prevItemStates[c.id]
                            var label = (c.iaId || c.title || "item")
                                        + (c.iaFile ? "/" + c.iaFile : "")
                            if (prev !== undefined && prev !== c.state) {
                                if (c.state === "mirrored")
                                    logEvent("mirror", "Preserved " + label)
                                else if (c.state === "mirroring")
                                    logEvent("info", "Preserving " + label + "…")
                                else if (c.state === "error")
                                    logEvent("error", "Preserve failed: " + label)
                                else if (c.state === "available" && prev === "mirrored")
                                    logEvent("info", "Removed " + label)
                            }
                            root._prevItemStates[c.id] = c.state
                        }
                        syncListModel(itemsModel, coNew.map(normalizeItem), "id")
                    }
                } catch (e2) {}
            } else {
                root.gatewayState = "offline"
                root.storageState = "offline"
            }
        } finally {
            root.pollBusy = false
        }
    }

    Timer {
        interval: 2500; repeat: true; running: true; triggeredOnStart: true
        onTriggered: root.poll()
    }

    // optimistic UI: flip a item's state locally the instant the user acts;
    // the next poll reconciles with the backend's truth
    function flipItemState(itemId, newState) {
        for (var i = 0; i < itemsModel.count; i++) {
            if (itemsModel.get(i).id === itemId) {
                itemsModel.setProperty(i, "state", newState)
                break
            }
        }
    }

    function gb(bytes) { return (bytes / 1e9).toFixed(bytes > 0 && bytes < 1e8 ? 2 : 1) }
    function prettySize(bytes) {
        if (!bytes || bytes <= 0) return "0 B"
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1048576) return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + " MB"
        return (bytes / 1073741824).toFixed(2) + " GB"
    }
    function shortId(s) { return s ? s.substring(0, 8) + "…" : "" }
    // node tx hashes are NOT valid explorer /txs links — prefer the block page,
    // which always resolves; the backend upgrades it to the real tx page on open
    function explorerUrl(col) {
        if (col && col.blockHash)
            return "https://logosblocks.noders.services/blocks/" + col.blockHash
        return "https://logosblocks.noders.services/txs/" + (col && col.txHash ? col.txHash : col)
    }
    function copyText(t) { clipHelper.text = t; clipHelper.selectAll(); clipHelper.copy(); clipHelper.text = "" }
    TextEdit { id: clipHelper; visible: false }

    // ── Activity log ─────────────────────────────────────────────────────────
    ListModel { id: activityModel }
    // one honest line per finished scan: what matched and why things were skipped (#11)
    function scanSummary(channelId) {
        var r = safeParse(logos.callModule("archive", "getScanDiagnostics", [channelId]), null)
        if (!r || !r.ok || !r.diagnostics) return ""
        var d = r.diagnostics
        var skips = []
        if (d.skippedNonJson)       skips.push(d.skippedNonJson + " non-json")
        if (d.skippedNoCid)         skips.push(d.skippedNoCid + " no-cid")
        if (d.skippedOversized)     skips.push(d.skippedOversized + " oversized")
        if (d.skippedDuplicate)     skips.push(d.skippedDuplicate + " duplicate")
        if (d.skippedNotFinalized)  skips.push(d.skippedNotFinalized + " not-finalized")
        if (d.oversizedBodies)      skips.push(d.oversizedBodies + " oversized-body")
        return " — scanned " + (d.scannedSlots >= 1000
                                ? Math.round(d.scannedSlots / 1000) + "k" : d.scannedSlots)
               + " slots: " + d.matched + " matched"
               + (skips.length ? ", skipped " + skips.join(", ") : "")
    }
    function logEvent(kind, text) {
        activityModel.append({ kind: kind, text: text,
                               ts: Qt.formatDateTime(new Date(), "hh:mm:ss") })
        if (activityModel.count > 100) activityModel.remove(0)   // keycard's cap, FIFO
        if (typeof activityList !== "undefined") activityList.positionViewAtEnd()
    }

    // ── Toast — immediate feedback for copy/share/preserve actions ───────────
    property string toastText: ""
    function toast(t) { toastText = t; toastTimer.restart() }
    Timer { id: toastTimer; interval: 2200; onTriggered: root.toastText = "" }

    // ── Share cards (SPEC §12): getShareData → ShareCard render → grabToImage →
    //    Canvas toDataURL → saveShareCard(pngBase64) → revealCard ────────────────
    property var shareData: null
    property bool shareBusy: false       // one export at a time — racing grabs would clobber
    function shareScope(scope) {
        if (shareBusy) return
        shareBusy = true
        toast("Rendering card…")
        call("getShareData", [scope], function(r) {
            if (!r || r.ok === false) {
                root.shareBusy = false
                root.toast(r && r.error === "nothing_preserved_yet"
                           ? "Preserve something first — the card shows what you keep alive"
                           : "Share failed: " + (r ? r.error : "no response"))
                return
            }
            root.shareData = r
            shareSettleTimer.restart()   // one tick for bindings to settle before the grab
        })
    }
    Timer {
        id: shareSettleTimer
        interval: 120
        onTriggered: {
            var ok = shareCard.grabToImage(function(result) {
                shareWatchdog.stop()
                var name = "ia-archive-" + (root.shareData && root.shareData.scope === "me"
                                             ? "contribution" : "item")
                           + "-" + Qt.formatDateTime(new Date(), "yyyyMMdd-hhmmss")
                var tmp = "/tmp/ia-archive-card-grab.png"
                if (!result.saveToFile(tmp)) {
                    root.shareBusy = false
                    root.toast("Share failed: could not write the grabbed image")
                    return
                }
                root.call("finalizeShareCard", [tmp, name], function(r) {
                    root.shareBusy = false
                    if (r && r.ok) {
                        root.logEvent("share", "Card saved — " + r.path)
                        root.toast("Card saved — opening folder")
                        root.call("revealCard", [r.path], null)
                    } else {
                        root.toast("Share failed: " + (r ? r.error : "save failed"))
                    }
                })
            }, Qt.size(1200, 675))
            if (!ok) {
                root.shareBusy = false
                root.toast("Share failed: card render rejected")
            } else {
                shareWatchdog.restart()   // grab callbacks can silently never fire
            }
        }
    }
    Timer {
        id: shareWatchdog
        interval: 8000
        onTriggered: {
            if (root.shareBusy) {
                root.shareBusy = false
                root.toast("Share timed out — try again")
            }
        }
    }

    function stateColor(s) {
        return s === "ready" ? successGreen
             : (s === "degraded" || s === "starting") ? warningYellow : errorRed
    }
    function mirrorColor(s) {
        return s === "mirrored" ? successGreen
             : s === "mirroring" ? warningYellow
             : s === "error" ? errorRed : textMuted
    }

    Rectangle { anchors.fill: parent; color: root.bgPrimary }

    // ── Styled controls (radio's dark components — default QtQuick chrome
    //    clashes with the dark theme) ─────────────────────────────────────────
    component DarkButton: Button {
        id: db
        hoverEnabled: true   // not on by default in the Basecamp host — no hover state without it
        contentItem: Text {
            text: db.text; font.pixelSize: 13
            color: !db.enabled ? root.textMuted : root.textPrimary
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6; implicitHeight: 32; implicitWidth: 72
            // a step brighter than any container it sits on (panes are #262626)
            color: !db.enabled ? "#2A2A2A"
                 : db.down ? "#1F1F1F"
                 : db.hovered ? "#4A4A4A" : "#383838"
            border.color: db.hovered && db.enabled ? "#5A5A5A" : "#4A4A4A"
            border.width: 1
            Behavior on color { ColorAnimation { duration: 80 } }
        }
    }
    component AccentButton: Button {
        id: ab
        hoverEnabled: true
        contentItem: Text {
            text: ab.text; font.pixelSize: 13; font.bold: true
            color: ab.enabled ? "#FFFFFF" : root.textMuted
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6; implicitHeight: 32; implicitWidth: 84
            color: !ab.enabled ? "#2A2A2A"
                 : ab.down ? "#CC4000"
                 : ab.hovered ? "#FF6A26" : root.accentOrange
            border.color: ab.enabled ? "transparent" : "#4A4A4A"
            border.width: ab.enabled ? 0 : 1
            Behavior on color { ColorAnimation { duration: 80 } }
        }
    }
    component DarkField: TextField {
        property color fieldBg: root.bgSecondary
        color: root.textPrimary
        placeholderTextColor: root.textMuted
        selectionColor: root.accentOrange
        background: Rectangle {
            radius: 6; implicitHeight: 32
            color: parent.fieldBg
            border.color: parent && parent.activeFocus ? root.accentOrange : root.borderColor
            border.width: 1
        }
    }
    component DarkRadio: RadioButton {
        id: dr
        spacing: 8
        font.pixelSize: 12
        palette.windowText: root.textSecondary
        indicator: Rectangle {
            implicitWidth: 18; implicitHeight: 18; radius: 9
            x: dr.leftPadding; y: dr.topPadding + (dr.availableHeight - height) / 2
            color: "transparent"; border.width: 2
            border.color: dr.checked ? root.accentOrange : root.textMuted
            Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4
                color: root.accentOrange; visible: dr.checked }
        }
    }

    // ── ShareCard — rendered offscreen at X/Twitter ratio, no PII ────────────
    Rectangle {
        id: shareCard
        width: 1200; height: 675
        x: -width * 2; y: 0            // parked offscreen; must stay visible for grabToImage
        color: "#101014"
        Rectangle {                     // subtle accent frame
            anchors.fill: parent; anchors.margins: 10
            color: "transparent"; border.color: root.accentOrange; border.width: 2; radius: 14
        }
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 56
            spacing: 18
            Label {
                text: root.shareData && root.shareData.scope !== "me"
                      ? (root.shareData.title || "A item")
                      : "I'm preserving the Internet Archive"
                color: "#FFFFFF"; font.pixelSize: 44; font.bold: true
                Layout.fillWidth: true; elide: Text.ElideRight
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 28
                Label {
                    text: root.shareData ? root.prettySize(root.shareData.totalBytes || 0).split(" ")[0] : "0"
                    color: root.accentOrange; font.pixelSize: 120; font.bold: true
                }
                ColumnLayout {
                    spacing: 4
                    Label {
                        text: (root.shareData ? root.prettySize(root.shareData.totalBytes || 0).split(" ")[1] : "GB")
                              + " preserved"
                        color: "#FFFFFF"; font.pixelSize: 30
                    }
                    Label {
                        text: root.shareData && root.shareData.scope === "me"
                              ? (root.shareData.count || 0) + " items" : "on decentralized Storage"
                        color: "#A4A4A4"; font.pixelSize: 22
                    }
                }
                Item { Layout.fillWidth: true }
            }
            // thumbnail grid: 1–6 covers; missing thumbnails get a tasteful placeholder
            RowLayout {
                Layout.fillWidth: true
                spacing: 14
                Repeater {
                    model: root.shareData ? Math.min(6, (root.shareData.items || []).length) : 0
                    Rectangle {
                        implicitWidth: 150; implicitHeight: 110; radius: 10
                        color: "#1E1E26"; border.color: "#383838"
                        clip: true
                        Image {
                            id: thumbImg
                            anchors.fill: parent
                            source: root.shareData.items[index].thumbnail || ""
                            fillMode: Image.PreserveAspectCrop
                            visible: status === Image.Ready
                        }
                        Label {   // placeholder: item initial
                            anchors.centerIn: parent
                            visible: thumbImg.status !== Image.Ready
                            text: (root.shareData.items[index].name || "?").substring(0, 1).toUpperCase()
                            color: root.accentOrange; font.pixelSize: 42; font.bold: true
                        }
                        Label {
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 6 }
                            text: root.shareData.items[index].name || ""
                            color: "#FFFFFF"; font.pixelSize: 12; elide: Text.ElideRight
                            visible: thumbImg.status !== Image.Ready
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "preserved & verifiable on the Logos Execution Zone"
                    color: "#A4A4A4"; font.pixelSize: 20
                }
                Item { Layout.fillWidth: true }
                Label { text: "logos.co"; color: root.accentOrange; font.pixelSize: 20; font.bold: true }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ── Header: title + pills + cogwheel ─────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "IA → λ"
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
                Label { text: "Gateway node"; color: root.textPrimary; font.bold: true; font.pixelSize: 13 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    DarkField {
                        id: nodeUrlField
                        Layout.fillWidth: true
                        fieldBg: root.bgPrimary
                        placeholderText: root.activeGatewayUrl || "node URL — http://gateway:8080"
                    }
                    DarkButton {
                        text: "Apply"
                        enabled: nodeUrlField.text.length > 0
                        onClicked: root.call("setGateways",
                            [JSON.stringify([{ nodeUrl: nodeUrlField.text }])],
                            function(r) { if (r && r.ok) root.logEvent("config", "Gateway set: " + nodeUrlField.text) })
                    }
                }
            }
        }

        // ── Summary counter — the campaign hook ──────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: summaryRow.implicitHeight + 16   // content-sized — the 32px
            radius: 8                                        // button overflowed a fixed 46
            color: root.bgSecondary; border.color: root.borderColor
            RowLayout {
                id: summaryRow
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 12; rightMargin: 12 }
                spacing: 10
                Label {
                    Layout.alignment: Qt.AlignVCenter
                    text: "You're preserving "
                          + (root.summary.mirrored || 0) + " item"
                          + ((root.summary.mirrored || 0) === 1 ? "" : "s")
                          + " · " + root.prettySize(root.summary.usedBytes || 0)
                    color: root.textPrimary; font.pixelSize: 14; font.bold: true
                }
                Item { Layout.fillWidth: true }
                Label {
                    Layout.alignment: Qt.AlignVCenter
                    text: "following " + (root.summary.following || 0)
                          + " · " + (root.summary.items || 0) + " items"
                    color: root.textSecondary; font.pixelSize: 11
                }
                AccentButton {
                    Layout.alignment: Qt.AlignVCenter
                    text: "Share"
                    enabled: (root.summary.mirrored || 0) > 0
                    onClicked: root.shareScope("me")
                }
            }
        }

        // ── Tabs ─────────────────────────────────────────────────────────────
        TabBar {
            id: tabs
            Layout.fillWidth: true
            background: Rectangle { color: root.bgPrimary }
            Repeater {
                model: ["Channels", "Items", "Activity"]
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
                    DarkField {
                        id: followField
                        Layout.fillWidth: true
                        placeholderText: "channel id, id@startSlot, or explorer URL"
                        onAccepted: followBtn.clicked()
                    }
                    AccentButton {
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
                    model: channelsModel
                    clip: true
                    spacing: 6
                    delegate: Rectangle {
                        width: ListView.view.width
                        implicitHeight: chRow.implicitHeight + 20
                        radius: 8; color: root.bgSecondary; border.color: root.borderColor
                        RowLayout {
                            id: chRow
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 12; rightMargin: 12 }
                            spacing: 8
                            ColumnLayout {
                                spacing: 2
                                RowLayout {
                                    spacing: 6
                                    Label {
                                        visible: !labelEdit.visible
                                        text: model.name || root.shortId(model.channelId) || "channel"
                                        color: root.textPrimary; font.pixelSize: 13; font.bold: true
                                    }
                                    DarkField {
                                        id: labelEdit
                                        visible: false
                                        implicitWidth: 180
                                        font.pixelSize: 12
                                        onAccepted: {
                                            var R = root, id = model.channelId, t = text
                                            visible = false
                                            R.call("setChannelLabel", [id, t], function(r) {
                                                if (r && r.ok) R.toast("Channel labeled \"" + t + "\"")
                                            })
                                        }
                                        Keys.onEscapePressed: visible = false
                                    }
                                    ToolButton {
                                        text: "✎"   // label this channel
                                        visible: !labelEdit.visible
                                        onClicked: {
                                            labelEdit.text = model.label || ""
                                            labelEdit.visible = true
                                            labelEdit.forceActiveFocus()
                                        }
                                        contentItem: Label { text: parent.text; color: root.textMuted; font.pixelSize: 12 }
                                        background: Rectangle { color: "transparent" }
                                    }
                                }
                                Label {
                                    text: (model.curator ? "curator " + root.shortId(model.curator) + " · " : "")
                                          + (model.items || 0) + " items"
                                    color: root.textSecondary; font.pixelSize: 11
                                }
                            }
                            Item { Layout.fillWidth: true }   // sole expander — pins actions to the edge
                            // right-aligned action group — consistent gaps
                            RowLayout {
                                spacing: 6
                                Layout.alignment: Qt.AlignVCenter
                                Rectangle {
                                    implicitWidth: syncLbl.implicitWidth + 12; implicitHeight: 20; radius: 10
                                    color: "transparent"; border.color: model.synced ? root.successGreen : root.warningYellow
                                    Label {
                                        id: syncLbl; anchors.centerIn: parent
                                        text: model.synced ? "synced"
                                              : "syncing " + (model.progress >= 0
                                                              ? model.progress + "%" : "…")
                                        color: model.synced ? root.successGreen : root.warningYellow
                                        font.pixelSize: 10
                                    }
                                }
                                ToolButton {
                                    // #17: preserve new items from this channel unattended
                                    text: "auto"
                                    onClicked: {
                                        var R = root, id = model.channelId, next = !model.autoPreserve
                                        R.call("setAutoPreserve", [id, next ? "true" : "false"], function(r) {
                                            if (r && r.ok) R.toast(next ? "Auto-preserve ON — new items download themselves"
                                                                        : "Auto-preserve off")
                                        })
                                    }
                                    contentItem: Label {
                                        text: parent.text
                                        color: model.autoPreserve ? root.successGreen : root.textMuted
                                        font.pixelSize: 11; font.bold: model.autoPreserve
                                    }
                                    background: Rectangle {
                                        color: "transparent"; radius: 3
                                        border.color: model.autoPreserve ? root.successGreen : root.borderColor
                                    }
                                }
                                ToolButton {
                                    text: "↻"
                                    enabled: !!model.channelId
                                    onClicked: {
                                        var R = root, id = model.channelId
                                        R.call("refreshChannel", [id], function(r) {
                                            if (r && r.ok) R.logEvent("refresh", "Refreshing " + R.shortId(id))
                                        })
                                    }
                                    contentItem: Label { text: parent.text; color: root.textSecondary; font.pixelSize: 14 }
                                    background: Rectangle { color: "transparent" }
                                }
                                ToolButton {
                                    text: "✕"
                                    enabled: !!model.channelId
                                    onClicked: {
                                        var R = root, id = model.channelId
                                        R.call("unfollowChannel", [id], function(r) {
                                            if (r && r.ok) R.logEvent("follow", "Unfollowed " + R.shortId(id))
                                        })
                                    }
                                    contentItem: Label { text: parent.text; color: root.errorRed; font.pixelSize: 12 }
                                    background: Rectangle { color: "transparent" }
                                }
                            }
                        }
                    }
                    Label {
                        visible: channelsModel.count === 0
                        anchors.centerIn: parent
                        text: "Follow a curated channel to see its items.\nReads need a synced gateway (LEZ#519)."
                        color: root.textMuted; font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ── Items tab ──────────────────────────────────────────────
            ColumnLayout {
                spacing: 8
                // #519 surfaced where it matters: this list may be missing recent inscriptions
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: root.gatewayState === "ready" ? 0 : staleLbl.implicitHeight + 16
                    visible: implicitHeight > 0
                    radius: 6; color: root.bgSecondary
                    border.color: root.gatewayState === "offline" ? root.errorRed : root.warningYellow
                    clip: true
                    Label {
                        id: staleLbl
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 10 }
                        text: root.gatewayState === "offline"
                              ? "Gateway unreachable — showing cached items; nothing here is live."
                              : "Gateway is lagging " + root.syncLag + " slots behind the chain — recent inscriptions may be missing."
                        color: root.gatewayState === "offline" ? root.errorRed : root.warningYellow
                        font.pixelSize: 11; wrapMode: Text.WordWrap
                    }
                }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    model: itemsModel
                    clip: true
                    spacing: 6
                    delegate: Rectangle {
                        width: ListView.view.width
                        implicitHeight: colCol.implicitHeight + 20
                        radius: 8; color: root.bgSecondary; border.color: root.borderColor
                        ColumnLayout {
                            id: colCol
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 12; rightMargin: 12 }
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: model.iaId || model.title || model.cid || "untitled"
                                    color: root.textPrimary; font.pixelSize: 13; font.bold: true
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    implicitWidth: stLbl.implicitWidth + 12; implicitHeight: 20; radius: 10
                                    color: "transparent"; border.color: root.mirrorColor(model.state)
                                    Label {
                                        id: stLbl; anchors.centerIn: parent
                                        text: (model.state || "available")
                                              + (model.state === "mirroring" && model.progressBlocks > 0
                                                 ? " " + model.progressBlocks + "%" : "")
                                        color: root.mirrorColor(model.state); font.pixelSize: 10
                                    }
                                }
                                DarkButton {
                                    text: "Copy IA link"
                                    enabled: !!model.iaId
                                    onClicked: {
                                        root.copyText("https://archive.org/details/" + model.iaId)
                                        root.toast("Internet Archive link copied")
                                    }
                                }
                                AccentButton {
                                    visible: model.state !== "mirrored"
                                    text: "Preserve"
                                    enabled: model.state !== "mirroring" && !!model.id
                                             && root.storageState === "ready"
                                    onClicked: {
                                        var R = root, t = model.iaId || model.title, id = model.id
                                        R.flipItemState(id, "mirroring")   // instant feedback
                                        R.toast("Preserving " + t + "…")
                                        R.call("mirrorItem", [id], function(r) {
                                            if (r && r.ok) R.logEvent("mirror", "Preserving " + t)
                                            else {
                                                R.flipItemState(id, "available")
                                                R.toast("Preserve failed: " + (r ? r.error : "no response"))
                                            }
                                        })
                                    }
                                }
                                DarkButton {
                                    // ia_item entries (no cid) have per-file CIDs in
                                    // storage — unpreserve isn't supported for them yet
                                    visible: model.state === "mirrored" && !!model.cid
                                    text: "Remove"
                                    Layout.preferredWidth: 84   // same footprint as Preserve
                                    enabled: !!model.id && root.storageState === "ready"
                                    onClicked: {
                                        var R = root, t = model.iaId || model.title
                                        R.call("unmirrorItem", [model.id], function(r) {
                                            if (r && r.ok) { R.logEvent("mirror", "Unpreserving " + t); R.toast("Unpreserved " + t) }
                                            else R.toast("Unpreserve failed: " + (r ? r.error : "no response"))
                                        })
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    visible: !!model.iaFile
                                    text: model.iaFile || ""
                                    color: root.textSecondary; font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                    Layout.maximumWidth: 220
                                }
                                Label {
                                    visible: !model.cid && !!model.iaId
                                    text: "from archive.org · checksum-verified"
                                    color: root.textMuted; font.pixelSize: 10
                                }
                                ToolButton {
                                    text: "tx " + root.shortId(model.txHash)
                                    enabled: !!model.txHash
                                    onClicked: {
                                        var R = root
                                        // the backend resolves the explorer's own tx hash
                                        // (node hash is a dead link there) — copy what it
                                        // actually opened, block page as the fallback
                                        var fallback = R.explorerUrl({ blockHash: model.blockHash,
                                                                       txHash: model.txHash })
                                        R.call("openExplorerTx",
                                               [model.txHash, model.blockHash || "",
                                                model.txIndex !== undefined ? model.txIndex : -1],
                                               function(r) {
                                            R.copyText(r && r.url ? r.url : fallback)
                                            R.toast(r && r.ok ? "Opening in explorer (link copied too)"
                                                              : "Explorer link copied")
                                        })
                                    }
                                    contentItem: Label { text: parent.text; color: root.accentOrange; font.pixelSize: 11 }
                                    background: Rectangle { color: "transparent" }
                                }
                                Label {
                                    text: "slot " + (model.inscribedAt || "?")
                                    color: root.textSecondary; font.pixelSize: 11
                                }
                                Label {
                                    visible: !!model.cid
                                    text: "CID " + root.shortId(model.cid)
                                    color: root.textMuted; font.pixelSize: 11
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                    Label {
                        visible: itemsModel.count === 0
                        anchors.centerIn: parent
                        text: "No items yet — follow a channel first."
                        color: root.textMuted; font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ── Activity tab — keycard-basecamp's ActivityLog pattern ────────
            Rectangle {
                color: root.bgPrimary
                Rectangle {   // top separator
                    anchors { top: parent.top; left: parent.left; right: parent.right }
                    height: 1; color: root.borderColor
                }
                // copy-all (keycard's two-rect clipboard glyph, top-right)
                Rectangle {
                    id: copyAllBtn
                    anchors { top: parent.top; right: parent.right; topMargin: 8; rightMargin: 8 }
                    width: 20; height: 20; color: "transparent"; z: 10
                    opacity: copyAllArea.containsMouse ? 0.8 : 0.5
                    Behavior on opacity { NumberAnimation { duration: 150 } }
                    Rectangle { x: 3; y: 5; width: 10; height: 10; color: "transparent"
                                border.color: root.textMuted; border.width: 1; radius: 2 }
                    Rectangle { x: 6; y: 2; width: 10; height: 10; color: root.bgPrimary
                                border.color: root.textMuted; border.width: 1; radius: 2 }
                    MouseArea {
                        id: copyAllArea
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var text = ""
                            for (var i = 0; i < activityModel.count; i++) {
                                var e = activityModel.get(i)
                                text += "[" + e.ts + "] " + e.text + "\n"
                            }
                            root.copyText(text)
                            copyAllBtn.opacity = 0.3
                            copyFlash.restart()
                            root.toast("Activity log copied")
                        }
                    }
                    ToolTip { visible: copyAllArea.containsMouse; text: "Copy all logs"; delay: 500 }
                    Timer { id: copyFlash; interval: 200
                            onTriggered: copyAllBtn.opacity = copyAllArea.containsMouse ? 0.8 : 0.5 }
                }
                ListView {
                    id: activityList
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 4
                    clip: true
                    model: activityModel
                    delegate: TextEdit {
                        width: activityList.width
                        text: "[" + model.ts + "] " + model.text
                        color: model.kind === "error" ? "#f44336"
                             : (model.kind === "mirror" || model.kind === "share") ? "#4caf50"
                             : model.kind === "gateway" ? "#ffc107" : "#888888"
                        font.pixelSize: 12
                        font.family: "monospace"
                        wrapMode: TextEdit.WordWrap
                        readOnly: true
                        selectByMouse: true
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

    // ── Toast overlay ────────────────────────────────────────────────────────
    Rectangle {
        visible: root.toastText !== ""
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 18 }
        width: Math.min(toastLbl.implicitWidth + 28, parent.width - 40); height: 34; radius: 17
        color: "#E6262626"; border.color: root.borderColor
        z: 1000
        Label {
            id: toastLbl; anchors.centerIn: parent
            text: root.toastText; color: root.textPrimary; font.pixelSize: 12
            elide: Text.ElideMiddle; width: Math.min(implicitWidth, parent.width - 24)
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
