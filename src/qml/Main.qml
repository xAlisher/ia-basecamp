import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// P3 fills this in (pills + Channels/Collections tabs + activity log). Skeleton verifies the
// QML↔backend wiring: logos.module("archive") → the QRO replica; logos.watch for async SLOTs.
Item {
    id: root
    width: 520; height: 680
    readonly property var backend: logos.module("archive")
    readonly property string gatewayState: backend ? backend.gatewayState : "no backend"
    readonly property int    syncLag:      backend ? backend.syncLagBlocks : 0

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 12
        Label { text: "Archive — follow & preserve"; font.pixelSize: 20; font.bold: true }
        RowLayout {
            spacing: 8
            Label { text: "Gateway: " + root.gatewayState + (root.syncLag > 0 ? " (lag " + root.syncLag + ")" : "") }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; opacity: 0.7
            text: "Skeleton (P0). Channels/Collections + Preserve land in P1–P3. Reads depend on a synced LEZ indexer — LEZ#519."
        }
        Item { Layout.fillHeight: true }
    }
}
