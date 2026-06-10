#include "archive_plugin.h"
#include "lez_client.h"
#include "storage_client.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QDebug>

namespace {

QString ok(QJsonObject extra = {})
{
    extra.insert(QStringLiteral("ok"), true);
    return QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact));
}

QString fail(const QString& code)
{
    const QJsonObject o{ { QStringLiteral("ok"), false }, { QStringLiteral("error"), code } };
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// the user's own node when preserveMode == "local" (Kubo default RPC port)
QString localStorageUrl()
{
    const QByteArray env = qgetenv("ARCHIVE_LOCAL_STORAGE_URL");
    return env.isEmpty() ? QStringLiteral("http://127.0.0.1:5001") : QString::fromUtf8(env);
}
} // namespace

ArchivePlugin::ArchivePlugin(QObject* parent) : ArchiveSimpleSource(parent) {}
ArchivePlugin::~ArchivePlugin() = default;

void ArchivePlugin::initLogos(LogosAPI* api)
{
    if (m_lez) return;
    m_logosAPI = api;
    // Pure HTTP clients — NO LogosModules (no platform-module dep → no delivery#31, runs anywhere).
    m_lez     = new LezClient(this);       // gateway node reads (SPEC §4.1)
    m_storage = new StorageClient(this);   // Storage pin/replicate (P2)

    connect(m_lez, &LezClient::healthChanged, this, [this](const QString& state, qint64 lag) {
        setGatewayState(state);
        setSyncLagBlocks(static_cast<int>(qMin<qint64>(lag, INT_MAX)));
    });
    connect(m_lez, &LezClient::channelsChanged, this, [this] { publishReadState(); });
    connect(m_lez, &LezClient::collectionsChanged, this, [this] { publishReadState(); });
    connect(m_lez, &LezClient::errorOccurred, this, [this](const QString& code) {
        setLastError(code);
    });

    connect(m_storage, &StorageClient::healthChanged, this, [this](const QString& state) {
        setStorageState(state);
    });
    connect(m_storage, &StorageClient::pinProgress, this, [this](const QString& cid, qint64 blocks) {
        m_lez->setCollectionState(m_cidToCollection.value(cid), QStringLiteral("mirroring"), blocks);
    });
    connect(m_storage, &StorageClient::pinFinished, this,
            [this](const QString& cid, bool pinOk, const QString& error) {
        const QString collectionId = m_cidToCollection.take(cid);
        if (pinOk) {
            m_lez->setCollectionState(collectionId, QStringLiteral("mirrored"));
            m_storage->queryRepoStat();
        } else {
            m_lez->setCollectionState(collectionId, QStringLiteral("error"));
            setLastError(error);
        }
    });
    connect(m_storage, &StorageClient::unpinFinished, this,
            [this](const QString& cid, bool unpinOk, const QString& error) {
        const QString collectionId = m_cidToCollection.take(cid);
        if (unpinOk) {
            m_lez->setCollectionState(collectionId, QStringLiteral("available"));
            m_storage->queryRepoStat();
        } else {
            m_lez->setCollectionState(collectionId, QStringLiteral("error"));
            setLastError(error);
        }
    });
    connect(m_storage, &StorageClient::repoStatResult, this, [this](qint64 usedBytes) {
        m_usedBytes = usedBytes;
        publishReadState();
    });

    m_lez->loadState();
    setPreserveMode(m_lez->preserveMode());
    publishReadState();
    setBackend(this);                      // register as the QRO source for the QML replica

    m_healthTimer = new QTimer(this);
    connect(m_healthTimer, &QTimer::timeout, this, [this]{ pollGatewayHealth(); });
    m_healthTimer->start(10000);
    QTimer::singleShot(1500, this, [this]{ pollGatewayHealth(); });
    qDebug() << "ArchivePlugin: initLogos done (HTTP-client backend)";
}

void ArchivePlugin::pollGatewayHealth()
{
    if (!m_lez)
        return;
    m_lez->pollHealth();
    syncStorageEndpoint();
    m_storage->pollHealth();
}

void ArchivePlugin::syncStorageEndpoint()
{
    const auto gws = m_lez->gateways();
    const QString gatewayStorage =
        gws.isEmpty() ? QString() : gws.at(m_lez->activeGateway()).storageUrl;
    m_storage->setEndpoint(StorageClient::resolveEndpoint(m_lez->preserveMode(), gatewayStorage,
                                                          localStorageUrl()));
}

void ArchivePlugin::publishReadState()
{
    setChannelsJson(QString::fromUtf8(QJsonDocument(m_lez->channelsJson()).toJson(QJsonDocument::Compact)));
    setCollectionsJson(QString::fromUtf8(QJsonDocument(m_lez->collectionsJson()).toJson(QJsonDocument::Compact)));
    QJsonObject summary = m_lez->summaryJson();
    summary.insert(QStringLiteral("usedBytes"), m_usedBytes);
    setSummaryJson(QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Compact)));
}

// ── config ──────────────────────────────────────────────────────────────────

QString ArchivePlugin::setGateways(QString jsonList)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonList.toUtf8());
    if (!doc.isArray())
        return fail(QStringLiteral("invalid_json"));
    QList<LezClient::Gateway> gws;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        // "nodeUrl" per SPEC §4.1; "indexerUrl" accepted for .rep comment compatibility
        QString node = o.value(QStringLiteral("nodeUrl")).toString();
        if (node.isEmpty())
            node = o.value(QStringLiteral("indexerUrl")).toString();
        if (node.isEmpty())
            return fail(QStringLiteral("gateway_missing_node_url"));
        const QUrl u(node);
        if (!u.isValid() || u.host().isEmpty()
            || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https"))
            || !u.query().isEmpty() || !u.fragment().isEmpty())
            return fail(QStringLiteral("invalid_gateway_url"));
        gws.append({ node, o.value(QStringLiteral("storageUrl")).toString() });
    }
    if (gws.isEmpty())
        return fail(QStringLiteral("empty_gateway_list"));
    m_lez->setGateways(gws);
    m_lez->pollHealth();   // immediate feedback on the new gateway
    return ok({ { QStringLiteral("count"), gws.size() } });
}

QString ArchivePlugin::choosePreserveMode(QString mode)
{
    if (mode != QLatin1String("delegate") && mode != QLatin1String("local"))
        return fail(QStringLiteral("invalid_mode"));
    m_lez->setPreserveMode(mode);
    setPreserveMode(mode);
    syncStorageEndpoint();
    m_storage->pollHealth();   // the mode switch changes which node we talk to
    return ok({ { QStringLiteral("mode"), mode } });
}

QString ArchivePlugin::getStatus()
{
    return ok({
        { QStringLiteral("gatewayState"), m_lez->gatewayState() },
        { QStringLiteral("syncLagBlocks"), m_lez->syncLagSlots() },
        { QStringLiteral("activeGateway"), m_lez->activeGateway() },
        { QStringLiteral("gateways"), m_lez->gateways().size() },
        { QStringLiteral("preserveMode"), m_lez->preserveMode() },
        { QStringLiteral("summary"), m_lez->summaryJson() },
    });
}

// ── channels ────────────────────────────────────────────────────────────────

QString ArchivePlugin::followChannel(QString channelRef)
{
    QString err;
    const QString id = m_lez->followChannel(channelRef, &err);
    if (id.isEmpty()) {
        setLastError(err);
        return fail(err);
    }
    return ok({ { QStringLiteral("channelId"), id } });
}

QString ArchivePlugin::unfollowChannel(QString channelId)
{
    if (!m_lez->unfollowChannel(channelId))
        return fail(QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("channelId"), channelId.toLower() } });
}

QString ArchivePlugin::refreshChannel(QString channelId)
{
    if (!m_lez->refreshChannel(channelId))
        return fail(m_lez->isFollowed(channelId.toLower())
                        ? QStringLiteral("refresh_in_progress")
                        : QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("channelId"), channelId.toLower() } });
}

QString ArchivePlugin::getCollections(QString channelId)
{
    if (!channelId.isEmpty() && !m_lez->isFollowed(channelId.toLower()))
        return fail(QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("collections"), m_lez->collectionsJson(channelId) } });
}

// ── preserve ────────────────────────────────────────────────────────────────

QString ArchivePlugin::mirrorCollection(QString collectionId)
{
    const QString cid = m_lez->collectionCid(collectionId);
    if (cid.isEmpty())
        return fail(QStringLiteral("unknown_collection"));
    // one in-flight storage op per cid — a second mirror, or an unmirror racing a
    // mirror, would corrupt the cid→collection ownership and the end state
    if (m_cidToCollection.contains(cid))
        return fail(QStringLiteral("storage_busy"));
    syncStorageEndpoint();
    if (m_storage->endpoint().isEmpty())
        return fail(QStringLiteral("no_storage_endpoint"));
    m_cidToCollection.insert(cid, collectionId);
    m_lez->setCollectionState(collectionId, QStringLiteral("mirroring"), 0);
    m_storage->pin(cid);
    return ok({ { QStringLiteral("collectionId"), collectionId },
                { QStringLiteral("cid"), cid },
                { QStringLiteral("mode"), m_lez->preserveMode() } });
}

QString ArchivePlugin::unmirrorCollection(QString collectionId)
{
    const QString cid = m_lez->collectionCid(collectionId);
    if (cid.isEmpty())
        return fail(QStringLiteral("unknown_collection"));
    if (m_cidToCollection.contains(cid))
        return fail(QStringLiteral("storage_busy"));
    syncStorageEndpoint();
    if (m_storage->endpoint().isEmpty())
        return fail(QStringLiteral("no_storage_endpoint"));
    m_cidToCollection.insert(cid, collectionId);
    m_storage->unpin(cid);
    return ok({ { QStringLiteral("collectionId"), collectionId } });
}

QString ArchivePlugin::getMirrorStatus(QString collectionId)
{
    const QString state = m_lez->collectionState(collectionId);
    if (state.isEmpty())
        return fail(QStringLiteral("unknown_collection"));
    return ok({ { QStringLiteral("collectionId"), collectionId },
                { QStringLiteral("state"), state },
                { QStringLiteral("storageState"), m_storage->storageState() } });
}
