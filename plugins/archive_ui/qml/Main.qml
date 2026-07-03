import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme    // logos-design-system palette tokens
import Logos.Controls // logos-design-system components (LogosButton/LogosTextField/LogosBadge) — skill: logos-design-system-adoption

// archive — follow curated LEZ channels, preserve items to Storage.
// Dark theme matching radio/keeper/stash. Sandbox rules (qml-sandbox-restrictions):
// no network/FileDialog/Qt.openUrlExternally here — all IO lives in the C++ backend;
// provenance links are copy-to-clipboard. Inside layouts use implicitHeight, never height.
Item {
    id: root
    width: 560; height: 720

    // ── Dark palette (radio/keeper) ──────────────────────────────────────────
    readonly property color bgPrimary:     Theme.palette.background
    readonly property color bgSecondary:   Theme.palette.backgroundSecondary
    readonly property color bgActive:      Theme.palette.surface
    readonly property color textPrimary:   Theme.palette.text
    readonly property color textSecondary: Theme.palette.textSecondary
    readonly property color textMuted:     Theme.palette.textMuted
    readonly property color accentOrange:  Theme.palette.primary
    readonly property color successGreen:  Theme.palette.success
    readonly property color warningYellow: Theme.palette.warning
    readonly property color errorRed:      Theme.palette.error
    readonly property color borderColor:   Theme.palette.border

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
    // display label for the internal state machine (#21) — the UI speaks "preserve",
    // the persisted state strings stay mirroring/mirrored (no migration)
    function stateLabel(s) {
        return s === "mirrored" ? "preserved"
             : s === "mirroring" ? "preserving"
             : s === "error" ? "failed" : (s || "available")
    }

    Rectangle { anchors.fill: parent; color: root.bgPrimary }

    // ── Controls: adopted from logos-design-system (Logos.Controls).
    //    Neutral LogosButton (delivery-demo convention — primary actions are
    //    neutral, accent is reserved for status/state), LogosTextField, LogosBadge.
    //    Semantic-colored glyph buttons (✕/auto/↻) stay custom ToolButtons below:
    //    LogosToolButton hardcodes its text color and can't express per-state hue.

    // ── ShareCard — rendered offscreen at X/Twitter ratio, no PII ────────────
    Rectangle {
        id: shareCard
        width: 1200; height: 675
        x: -width * 2; y: 0            // parked offscreen; must stay visible for grabToImage
        color: Theme.palette.backgroundElevated
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
                color: root.textPrimary; font.pixelSize: 44; font.bold: true
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
                        color: root.textPrimary; font.pixelSize: 30
                    }
                    Label {
                        text: root.shareData && root.shareData.scope === "me"
                              ? (root.shareData.count || 0) + " items" : "on decentralized Storage"
                        color: root.textSecondary; font.pixelSize: 22
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
                        color: Theme.palette.surfaceRaised; border.color: Theme.palette.borderSubtle
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
                            color: root.textPrimary; font.pixelSize: 12; elide: Text.ElideRight
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
                    color: root.textSecondary; font.pixelSize: 20
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

            // Gateway pill — surfaces LEZ#519 lag directly (LogosBadge: state-colored)
            LogosBadge {
                text: "Gateway " + root.gatewayState
                      + (root.gatewayState === "degraded" ? " · lag " + root.syncLag : "")
                color: root.stateColor(root.gatewayState)
            }
            LogosBadge {
                text: "Storage " + root.storageState
                color: root.stateColor(root.storageState)
            }
            // Settings — canonical design-system icon button (LogosIconButton +
            // gear SVG; Logos.Icons ships no gear). Active state = accent tint.
            // SVG is inlined as a data: URI — the .lgx bundler ships only the view
            // file (Main.qml), so a loose qml/icons/*.svg asset would be dropped.
            LogosIconButton {
                readonly property string gearSvg: "data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMjAiIGhlaWdodD0iMjAiIHZpZXdCb3g9IjAgMCAxNiAxNiIgZmlsbD0ibm9uZSIgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIj4KPHBhdGggZD0iTTkuNDA1IDEuMDVjLS40MTMtMS40LTIuMzk3LTEuNC0yLjgxIDBsLS4xLjM0YTEuNDY0IDEuNDY0IDAgMCAxLTIuMTA1Ljg3MmwtLjMxLS4xN2MtMS4yODMtLjY5OC0yLjY4Ni43MDUtMS45ODcgMS45ODdsLjE2OS4zMTFjLjQ0Ni44Mi4wMjMgMS44NDEtLjg3MiAyLjEwNWwtLjM0LjFjLTEuNC40MTMtMS40IDIuMzk3IDAgMi44MWwuMzQuMWExLjQ2NCAxLjQ2NCAwIDAgMSAuODcyIDIuMTA1bC0uMTcuMzFjLS42OTggMS4yODMuNzA1IDIuNjg2IDEuOTg3IDEuOTg3bC4zMTEtLjE2OWExLjQ2NCAxLjQ2NCAwIDAgMSAyLjEwNS44NzJsLjEuMzRjLjQxMyAxLjQgMi4zOTcgMS40IDIuODEgMGwuMS0uMzRhMS40NjQgMS40NjQgMCAwIDEgMi4xMDUtLjg3MmwuMzEuMTdjMS4yODMuNjk4IDIuNjg2LS43MDUgMS45ODctMS45ODdsLS4xNjktLjMxMWExLjQ2NCAxLjQ2NCAwIDAgMSAuODcyLTIuMTA1bC4zNC0uMWMxLjQtLjQxMyAxLjQtMi4zOTcgMC0yLjgxbC0uMzQtLjFhMS40NjQgMS40NjQgMCAwIDEtLjg3Mi0yLjEwNWwuMTctLjMxYy42OTgtMS4yODMtLjcwNS0yLjY4Ni0xLjk4Ny0xLjk4N2wtLjMxMS4xNjlhMS40NjQgMS40NjQgMCAwIDEtMi4xMDUtLjg3MmwtLjEtLjM0ek04IDEwLjkzYTIuOTI5IDIuOTI5IDAgMSAxIDAtNS44NTggMi45MjkgMi45MjkgMCAwIDEgMCA1Ljg1OHoiIGZpbGw9IiM5Njk2OTYiLz4KPC9zdmc+Cg=="
                iconSource: gearSvg
                iconColor: root.settingsOpen ? root.accentOrange : root.textSecondary
                size: 28; iconSize: 16
                onClicked: root.settingsOpen = !root.settingsOpen
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
                    LogosTextField {
                        id: nodeUrlField
                        Layout.fillWidth: true
                        placeholderText: root.activeGatewayUrl || "node URL — http://gateway:8080"
                    }
                    LogosButton {
                        text: "Apply"
                        implicitWidth: 72; implicitHeight: 40
                        enabled: nodeUrlField.text.length > 0
                        onClicked: root.call("setGateways",
                            [JSON.stringify([{ nodeUrl: nodeUrlField.text }])],
                            function(r) { if (r && r.ok) root.logEvent("config", "Gateway set: " + nodeUrlField.text) })
                    }
                }
                // ── Channels (#26) — follow + manage from Settings ──────────────
                Label { text: "Channels"; color: root.textSecondary; font.bold: true; font.pixelSize: 13; topPadding: 6 }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    LogosTextField {
                        id: followField
                        Layout.fillWidth: true
                        placeholderText: "channel id, id@startSlot, or explorer URL"
                        // LogosTextField has no `accepted` signal — wire the inner TextInput
                        Connections { target: followField.textInput; function onAccepted() { followBtn.clicked() } }
                    }
                    LogosButton {
                        id: followBtn
                        text: "Follow"
                        implicitWidth: 84; implicitHeight: 40
                        enabled: followField.text.length > 0
                        onClicked: root.call("followChannel", [followField.text], function(r) {
                            if (r && r.ok) { root.logEvent("follow", "Following " + root.shortId(r.channelId)); followField.text = "" }
                        })
                    }
                }
                Repeater {
                    model: channelsModel
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: chRow.implicitHeight + 12
                        radius: 6; color: root.bgPrimary; border.color: root.borderColor
                        RowLayout {
                            id: chRow
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 10; rightMargin: 10 }
                            spacing: 8
                            Label {
                                visible: !chEdit.visible
                                Layout.fillWidth: true
                                text: (model.name || root.shortId(model.channelId) || "channel")
                                      + "  ·  " + (model.synced ? "synced" : "syncing " + (model.progress >= 0 ? model.progress + "%" : "…"))
                                      + "  ·  " + (model.items || 0) + " items"
                                color: root.textSecondary; font.pixelSize: 11; elide: Text.ElideRight
                            }
                            LogosTextField {
                                id: chEdit                 // #33: custom channel label
                                visible: false
                                Layout.fillWidth: true
                                placeholderText: "custom label"
                                Connections {
                                    target: chEdit.textInput
                                    function onAccepted() {
                                        var R = root, id = model.channelId, t = chEdit.text
                                        chEdit.visible = false
                                        R.call("setChannelLabel", [id, t], function(r) {
                                            if (r && r.ok) R.toast("Channel labeled \"" + t + "\"")
                                        })
                                    }
                                }
                                Keys.onEscapePressed: chEdit.visible = false
                            }
                            ToolButton {                   // #33: ✎ edit label
                                text: "✎"
                                visible: !chEdit.visible
                                onClicked: { chEdit.text = model.label || model.name || ""; chEdit.visible = true; chEdit.textInput.forceActiveFocus() }
                                contentItem: Label { text: parent.text; color: root.textMuted; font.pixelSize: 12 }
                                background: Rectangle { color: "transparent" }
                            }
                            ToolButton {
                                text: "auto"
                                onClicked: {
                                    var R = root, id = model.channelId, next = !model.autoPreserve
                                    R.call("setAutoPreserve", [id, next ? "true" : "false"], function(r) {
                                        if (r && r.ok) R.toast(next ? "Auto-preserve ON" : "Auto-preserve off")
                                    })
                                }
                                contentItem: Label { text: parent.text; color: model.autoPreserve ? root.successGreen : root.textMuted; font.pixelSize: 11; font.bold: model.autoPreserve }
                                background: Rectangle { color: "transparent"; radius: 3; border.color: model.autoPreserve ? root.successGreen : root.borderColor }
                            }
                            ToolButton {
                                text: "↻"
                                onClicked: { var R = root, id = model.channelId; R.call("refreshChannel", [id], function(r){ if (r && r.ok) R.logEvent("refresh", "Refreshing " + R.shortId(id)) }) }
                                contentItem: Label { text: parent.text; color: root.textSecondary; font.pixelSize: 14 }
                                background: Rectangle { color: "transparent" }
                            }
                            ToolButton {
                                text: "✕"
                                onClicked: { var R = root, id = model.channelId; R.call("unfollowChannel", [id], function(r){ if (r && r.ok) R.logEvent("follow", "Unfollowed " + R.shortId(id)) }) }
                                contentItem: Label { text: parent.text; color: root.errorRed; font.pixelSize: 12 }
                                background: Rectangle { color: "transparent" }
                            }
                        }
                    }
                }

                // ── Remove all (#24) — forget every followed channel + its items ──
                Label { text: "Danger zone"; color: root.textSecondary; font.bold: true; font.pixelSize: 13; topPadding: 6 }
                LogosButton {
                    id: removeAllBtn
                    property bool armed: false
                    text: armed ? "Confirm — remove all items?" : "Remove all items"
                    implicitWidth: 220; implicitHeight: 36
                    onClicked: {
                        if (!armed) { armed = true; return }
                        armed = false
                        root.call("removeAllItems", [], function(r) {
                            if (r && r.ok) root.logEvent("config", "Removed all items — " + (r.removed || 0) + " channel(s) forgotten")
                            else root.toast("Remove all failed: " + (r ? r.error : "no response"))
                        })
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
                LogosButton {
                    Layout.alignment: Qt.AlignVCenter
                    text: "Share"
                    implicitWidth: 84; implicitHeight: 32
                    visible: false   // hidden — share cards out of scope for the campaign UI
                    enabled: (root.summary.mirrored || 0) > 0
                    onClicked: root.shareScope("me")
                }
            }
        }

        // ── Items (main view) ────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
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
                        implicitHeight: itemRow.implicitHeight + 22
                        radius: 8; color: root.bgSecondary; border.color: root.borderColor

                        // ── small red ✕ — only on preserved items (#4); arms a
                        //    left-side "Remove?" confirm (#3), top-right corner ──
                        ToolButton {
                            id: rmBtn
                            anchors { top: parent.top; right: parent.right; topMargin: 2; rightMargin: 4 }
                            width: 22; height: 22; z: 5
                            // #32: also show during mirroring/pending (press = abort)
                            visible: model.state === "mirrored" || model.state === "mirroring" || model.state === "pending"
                            property bool armed: false
                            text: "✕"
                            onClicked: {
                                if (model.state === "mirroring" || model.state === "pending") {
                                    var R = root, id = model.id, t = model.iaId || model.title
                                    R.flipItemState(id, "available")   // instant reset
                                    R.call("abortPreserve", [id], function(r) {
                                        if (r && r.ok) R.toast("Aborted " + t)
                                    })
                                } else {                                // mirrored → confirm remove
                                    armed = true; rmArm.restart()
                                }
                            }
                            Timer { id: rmArm; interval: 3000; onTriggered: rmBtn.armed = false }
                            contentItem: Label { text: parent.text; color: root.errorRed
                                                 font.pixelSize: 13; font.bold: true
                                                 horizontalAlignment: Text.AlignHCenter }
                            background: Rectangle { color: "transparent" }
                        }

                        RowLayout {
                            id: itemRow
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 12; rightMargin: 30 }
                            spacing: 10

                            // ── LEFT: title + Copy link, then ch / slot / tx ids ──
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Label {
                                        text: model.iaId || model.title || model.cid || "untitled"
                                        color: root.textPrimary; font.pixelSize: 13; font.bold: true
                                        elide: Text.ElideRight; Layout.maximumWidth: 280
                                    }
                                    Label {
                                        visible: !!model.iaId
                                        text: "Copy link"
                                        color: linkMa.containsMouse ? root.accentOrange : root.textMuted
                                        font.pixelSize: 11
                                        MouseArea { id: linkMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: { root.copyText("https://archive.org/details/" + model.iaId); root.toast("Internet Archive link copied") } }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14
                                    Label {
                                        visible: !!model.channelId
                                        text: "ch: " + root.shortId(model.channelId)
                                        color: chMa.containsMouse ? root.accentOrange : root.textMuted; font.pixelSize: 10
                                        MouseArea { id: chMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: { root.copyText(model.channelId); root.toast("Channel id copied") } }
                                    }
                                    Label {
                                        text: "slot: " + (model.inscribedAt || "?")
                                        color: slotMa.containsMouse ? root.accentOrange : root.textMuted; font.pixelSize: 10
                                        MouseArea { id: slotMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: { root.copyText("" + (model.inscribedAt || "")); root.toast("Slot copied") } }
                                    }
                                    Label {
                                        visible: !!model.txHash
                                        text: "tx: " + root.shortId(model.txHash)
                                        color: txMa.containsMouse ? root.accentOrange : root.textMuted; font.pixelSize: 10
                                        MouseArea { id: txMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                var R = root
                                                var fallback = R.explorerUrl({ blockHash: model.blockHash, txHash: model.txHash })
                                                R.call("openExplorerTx", [model.txHash, model.blockHash || "", model.txIndex !== undefined ? model.txIndex : -1], function(r) {
                                                    R.copyText(r && r.url ? r.url : fallback); R.toast("Explorer tx link copied")
                                                })
                                            } }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }

                            // ── RIGHT: fixed-size state widget — identical footprint, colour per state ──
                            Rectangle {
                                id: stateW
                                Layout.preferredWidth: 116; Layout.preferredHeight: 30; Layout.alignment: Qt.AlignVCenter
                                radius: 6
                                property string st: model.state || "available"
                                color: st === "available" ? root.accentOrange : "transparent"
                                border.width: st === "available" ? 0 : 1
                                border.color: (st === "mirroring" || st === "pending") ? root.warningYellow
                                            : st === "mirrored" ? root.successGreen
                                            : st === "error" ? root.errorRed : root.borderColor
                                // #36: dim the Preserve pill while storage is offline — the
                                // click is already gated, this shows it. The breathing
                                // animation below overrides opacity while running, then this
                                // binding is restored (st is no longer available/error).
                                opacity: ((st === "available" || st === "error")
                                          && root.storageState !== "ready") ? 0.4 : 1.0
                                Label {
                                    anchors.centerIn: parent
                                    text: stateW.st === "available" ? "Preserve"
                                        : stateW.st === "mirroring" ? ((model.progressBlocks > 0 ? model.progressBlocks : 0) + "%")
                                        : stateW.st === "pending" ? "Pending"
                                        : stateW.st === "mirrored" ? "Preserved"
                                        : stateW.st === "error" ? "Error" : stateW.st
                                    color: stateW.st === "available" ? root.textPrimary
                                         : (stateW.st === "mirroring" || stateW.st === "pending") ? root.warningYellow
                                         : stateW.st === "mirrored" ? root.successGreen
                                         : stateW.st === "error" ? root.errorRed : root.textMuted
                                    font.pixelSize: 12; font.bold: true
                                }
                                // softly breathing while in-progress or pending-retry
                                SequentialAnimation on opacity {
                                    running: stateW.st === "mirroring" || stateW.st === "pending"
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.45; duration: 900; easing.type: Easing.InOutSine }
                                    NumberAnimation { to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: (stateW.st === "available" || stateW.st === "error") && !!model.id && root.storageState === "ready"
                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        var R = root, t = model.iaId || model.title, id = model.id
                                        R.flipItemState(id, "mirroring")
                                        R.toast("Preserving " + t + "…")
                                        R.call("mirrorItem", [id], function(r) {
                                            if (r && r.ok) R.logEvent("mirror", "Preserving " + t)
                                            else { R.flipItemState(id, "available"); R.toast("Preserve failed: " + (r ? r.error : "no response")) }
                                        })
                                    }
                                }
                            }

                            // ── #3: red "Remove?" confirm on the far right when ✕ is armed ──
                            Rectangle {
                                visible: rmBtn.armed
                                Layout.preferredWidth: 84; Layout.preferredHeight: 30; Layout.alignment: Qt.AlignVCenter
                                radius: 6; color: root.errorRed
                                Label { anchors.centerIn: parent; text: "Remove?"; color: root.textPrimary; font.pixelSize: 12; font.bold: true }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        // #35: unpreserve — delete from storage, reset to
                                        // Preserve (line stays). Optimistic flip; the 2s
                                        // poll reconciles if the backend rejects it.
                                        rmBtn.armed = false
                                        var R = root, id = model.id, t = model.iaId || model.title
                                        R.flipItemState(id, "available")
                                        R.call("unmirrorItem", [id], function(r) {
                                            if (r && r.ok) R.logEvent("config", "Removed " + t + " from storage")
                                            else R.toast("Remove failed: " + (r ? r.error : "no response"))
                                        })
                                    }
                                }
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

            // ── Activity log — docked at the bottom (keycard ActivityLog) ────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 150
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
                        color: model.kind === "error" ? root.errorRed
                             : (model.kind === "mirror" || model.kind === "share") ? root.successGreen
                             : model.kind === "gateway" ? root.warningYellow : root.textSecondary
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

    // ── Toast overlay ────────────────────────────────────────────────────────
    Rectangle {
        visible: root.toastText !== ""
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 18 }
        width: Math.min(toastLbl.implicitWidth + 28, parent.width - 40); height: 34; radius: 17
        color: Qt.rgba(root.bgSecondary.r, root.bgSecondary.g, root.bgSecondary.b, 0.9); border.color: root.borderColor
        z: 1000
        Label {
            id: toastLbl; anchors.centerIn: parent
            text: root.toastText; color: root.textPrimary; font.pixelSize: 12
            elide: Text.ElideMiddle; width: Math.min(implicitWidth, parent.width - 24)
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
