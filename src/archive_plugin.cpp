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

const char* kNotImpl = R"({"ok":false,"error":"not_implemented"})";

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
    if (m_lez)
        m_lez->pollHealth();
}

void ArchivePlugin::publishReadState()
{
    setChannelsJson(QString::fromUtf8(QJsonDocument(m_lez->channelsJson()).toJson(QJsonDocument::Compact)));
    setCollectionsJson(QString::fromUtf8(QJsonDocument(m_lez->collectionsJson()).toJson(QJsonDocument::Compact)));
    setSummaryJson(QString::fromUtf8(QJsonDocument(m_lez->summaryJson()).toJson(QJsonDocument::Compact)));
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

// ── preserve (P2: storage_client) ───────────────────────────────────────────
QString ArchivePlugin::mirrorCollection(QString)   { return kNotImpl; }
QString ArchivePlugin::unmirrorCollection(QString) { return kNotImpl; }
QString ArchivePlugin::getMirrorStatus(QString)    { return kNotImpl; }
