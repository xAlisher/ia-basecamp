#include "archive_plugin.h"
#include "lez_client.h"
#include "storage_client.h"
#include <QTimer>
#include <QDebug>

namespace { const char* kNotImpl = R"({"ok":false,"error":"not_implemented"})"; }

ArchivePlugin::ArchivePlugin(QObject* parent) : ArchiveSimpleSource(parent) {}
ArchivePlugin::~ArchivePlugin() = default;

void ArchivePlugin::initLogos(LogosAPI* api)
{
    if (m_lez) return;
    m_logosAPI = api;
    // Pure HTTP clients — NO LogosModules (no platform-module dep → no delivery#31, runs anywhere).
    m_lez     = new LezClient(this);       // LEZ indexer JSON-RPC reads
    m_storage = new StorageClient(this);   // Storage pin/replicate (delegate|local)
    setBackend(this);                      // register as the QRO source for the QML replica

    m_healthTimer = new QTimer(this);
    connect(m_healthTimer, &QTimer::timeout, this, [this]{ pollGatewayHealth(); });
    m_healthTimer->start(10000);
    QTimer::singleShot(1500, this, [this]{ pollGatewayHealth(); });
    qDebug() << "ArchivePlugin: initLogos done (HTTP-client backend)";
}

void ArchivePlugin::pollGatewayHealth() { /* P1: indexer health + sync lag vs tip → setSyncLagBlocks/setGatewayState */ }

// ── config (P1) ─────────────────────────────────────────────────────────────
QString ArchivePlugin::setGateways(QString)      { return kNotImpl; }
QString ArchivePlugin::choosePreserveMode(QString) { return kNotImpl; }
QString ArchivePlugin::getStatus()               { return kNotImpl; }
// ── channels (P1: lez_client.getTransactionsByAccount + decode ChannelInscribe) ──
QString ArchivePlugin::followChannel(QString)    { return kNotImpl; }
QString ArchivePlugin::unfollowChannel(QString)  { return kNotImpl; }
QString ArchivePlugin::refreshChannel(QString)   { return kNotImpl; }
QString ArchivePlugin::getCollections(QString)   { return kNotImpl; }
// ── preserve (P2: storage_client) ───────────────────────────────────────────
QString ArchivePlugin::mirrorCollection(QString)   { return kNotImpl; }
QString ArchivePlugin::unmirrorCollection(QString) { return kNotImpl; }
QString ArchivePlugin::getMirrorStatus(QString)    { return kNotImpl; }
