#include "ArchivePlugin.h"
#include "lez_client.h"
#include "logos_storage_transport.h"
#include "share_helper.h"
#include "storage_client.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
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
} // namespace

ArchivePlugin::ArchivePlugin(QObject* parent) : QObject(parent) {}
ArchivePlugin::~ArchivePlugin() = default;

void ArchivePlugin::initLogos(LogosAPI* api)
{
    if (m_lez) return;
    m_logosAPI = api;
    logosAPI = api;   // PluginInterface's public member — QtProviderObject refuses every
                      // callMethod ("LogosAPI not available") until this is set
    m_lez = new LezClient(this);           // gateway node reads (SPEC §4.1) — pure HTTP
    // Preserve goes ONLY to Logos Storage via the platform storage_module — from
    // logos_host, the proven stash path. The guard stays for safety (a mismatched
    // host must degrade to offline storage, never die).
    try {
        m_transport.reset(new LogosStorageTransport(api));
    } catch (const std::exception& e) {
        qWarning() << "ArchivePlugin: storage transport unavailable (SDK/host mismatch):"
                   << e.what();
        m_transport.reset(new NullStorageTransport());
    }
    m_storage = new StorageClient(m_transport.get(), this);

    connect(m_lez, &LezClient::errorOccurred, this, [this](const QString& code) {
        m_lastError = code;
    });
    connect(m_storage, &StorageClient::pinFinished, this,
            [this](const QString& cid, bool pinOk, const QString& error) {
        const QString collectionId = m_cidToCollection.take(cid);
        if (pinOk) {
            m_lez->setCollectionState(collectionId, QStringLiteral("mirrored"));
            m_storage->queryRepoStat();
            return;
        }
        // nobody on the network provides this CID — keeper's flow: pull the file
        // from the Internet Archive and upload it, making US the provider
        startReseed(collectionId, cid);
        Q_UNUSED(error);
    });
    connect(m_storage, &StorageClient::uploadFinished, this,
            [this](bool upOk, const QString& cid, const QString& error) {
        if (m_reseedCollectionId.isEmpty())
            return;
        const QString collectionId = m_reseedCollectionId;
        m_reseedCollectionId.clear();
        if (!m_reseedTmpPath.isEmpty()) {
            QFile::remove(m_reseedTmpPath);
            m_reseedTmpPath.clear();
        }
        if (!upOk) {
            m_lez->setCollectionState(collectionId, QStringLiteral("error"));
            m_lastError = QStringLiteral("reseed_upload_failed: ") + error;
            qWarning() << "ArchivePlugin: reseed upload failed for" << collectionId << error;
            return;
        }
        qInfo() << "ArchivePlugin: reseed complete —" << collectionId << "now provided as" << cid;
        if (!cid.isEmpty() && cid != m_reseedCid)
            qWarning() << "ArchivePlugin: reseeded CID differs from inscribed:" << cid
                       << "vs" << m_reseedCid;   // content drifted at the source — still held
        m_lez->setCollectionState(collectionId, QStringLiteral("mirrored"));
        m_storage->queryRepoStat();
        m_reseedCid.clear();
    });
    connect(m_storage, &StorageClient::unpinFinished, this,
            [this](const QString& cid, bool unpinOk, const QString& error) {
        const QString collectionId = m_cidToCollection.take(cid);
        if (unpinOk) {
            m_lez->setCollectionState(collectionId, QStringLiteral("available"));
            m_storage->queryRepoStat();
        } else {
            m_lez->setCollectionState(collectionId, QStringLiteral("error"));
            m_lastError = error;
        }
    });
    connect(m_storage, &StorageClient::repoStatResult, this,
            [this](qint64 usedBytes) { m_usedBytes = usedBytes; });
    // a long history is many capped refreshes — chain them so "follow" syncs to lib
    // without the user hammering refresh; stop while the gateway is down
    connect(m_lez, &LezClient::scanFinished, this,
            [this](const QString& channelId, bool reachedLib) {
        if (reachedLib || !m_lez->isFollowed(channelId))
            return;
        if (m_lez->gatewayState() == QLatin1String("offline"))
            return;   // the next health poll's refresh attempt picks it back up
        QTimer::singleShot(1200, this, [this, channelId] {
            if (m_lez->isFollowed(channelId)
                && m_lez->gatewayState() != QLatin1String("offline"))
                m_lez->refreshChannel(channelId);
        });
    });

    m_lez->loadState();

    m_healthTimer = new QTimer(this);
    connect(m_healthTimer, &QTimer::timeout, this, [this]{ pollGatewayHealth(); });
    m_healthTimer->start(10000);
    QTimer::singleShot(1500, this, [this]{ pollGatewayHealth(); });
    QTimer::singleShot(0, this, [this] {
        const QString dataDir =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/ia-archive/storage");
        m_storage->initStorage(dataDir);
    });
    qDebug() << "ArchivePlugin: initLogos done (core module, stash shape)";
}

void ArchivePlugin::pollGatewayHealth()
{
    if (!m_lez)
        return;
    m_lez->pollHealth();
    m_storage->pollHealth();
    // usedBytes resets with the process — keep the campaign counter live, not
    // only after the next preserve
    if (m_storage->storageState() == QLatin1String("ready"))
        m_storage->queryRepoStat();
}

// ── status ──────────────────────────────────────────────────────────────────

QString ArchivePlugin::getStatus()
{
    if (!m_lez)
        return fail(QStringLiteral("not_initialized"));
    QJsonObject summary = m_lez->summaryJson();
    summary.insert(QStringLiteral("usedBytes"), m_usedBytes);
    const auto gws = m_lez->gateways();
    const QString activeUrl =
        gws.isEmpty() ? QString() : gws.at(m_lez->activeGateway()).nodeUrl;
    return ok({
        { QStringLiteral("activeGatewayUrl"), activeUrl },
        { QStringLiteral("gatewayState"), m_lez->gatewayState() },
        { QStringLiteral("syncLagBlocks"), m_lez->syncLagSlots() },
        { QStringLiteral("storageState"), m_storage->storageState() },
        { QStringLiteral("preserveMode"), m_lez->preserveMode() },
        { QStringLiteral("lastError"), m_lastError },
        { QStringLiteral("activeGateway"), m_lez->activeGateway() },
        { QStringLiteral("gateways"), m_lez->gateways().size() },
        { QStringLiteral("summary"), summary },
    });
}

// ── config ──────────────────────────────────────────────────────────────────

QString ArchivePlugin::setGateways(const QString& jsonList)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonList.toUtf8());
    if (!doc.isArray())
        return fail(QStringLiteral("invalid_json"));
    QList<LezClient::Gateway> gws;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
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
    m_lez->pollHealth();
    return ok({ { QStringLiteral("count"), gws.size() } });
}

QString ArchivePlugin::choosePreserveMode(const QString& mode)
{
    if (mode != QLatin1String("delegate") && mode != QLatin1String("local"))
        return fail(QStringLiteral("invalid_mode"));
    m_lez->setPreserveMode(mode);
    return ok({ { QStringLiteral("mode"), mode } });
}

// ── channels ────────────────────────────────────────────────────────────────

QString ArchivePlugin::followChannel(const QString& channelRef)
{
    QString err;
    const QString id = m_lez->followChannel(channelRef, &err);
    if (id.isEmpty()) {
        m_lastError = err;
        return fail(err);
    }
    return ok({ { QStringLiteral("channelId"), id } });
}

QString ArchivePlugin::unfollowChannel(const QString& channelId)
{
    if (!m_lez->unfollowChannel(channelId))
        return fail(QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("channelId"), channelId.toLower() } });
}

QString ArchivePlugin::refreshChannel(const QString& channelId)
{
    if (!m_lez->refreshChannel(channelId))
        return fail(m_lez->isFollowed(channelId.toLower())
                        ? QStringLiteral("refresh_in_progress")
                        : QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("channelId"), channelId.toLower() } });
}

QString ArchivePlugin::getChannels()
{
    return ok({ { QStringLiteral("channels"), m_lez->channelsJson() } });
}

QString ArchivePlugin::getScanDiagnostics(const QString& channelId)
{
    const QJsonObject diag = m_lez->scanDiagnosticsJson(channelId);
    if (!diag.value(QStringLiteral("known")).toBool())
        return fail(QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("diagnostics"), diag } });
}

QString ArchivePlugin::setChannelLabel(const QString& channelId, const QString& label)
{
    if (!m_lez->setChannelLabel(channelId, label))
        return fail(QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("channelId"), channelId.toLower() },
                { QStringLiteral("label"), label.left(64) } });
}

// ── collections + preserve ──────────────────────────────────────────────────

void ArchivePlugin::startReseed(const QString& collectionId, const QString& cid)
{
    // resolve the IA source candidates from the inscription's keeper conventions
    QString title, manCid;
    for (const QJsonValue& v : m_lez->collectionsJson()) {
        const QJsonObject c = v.toObject();
        if (c.value(QStringLiteral("id")).toString() == collectionId) {
            title = c.value(QStringLiteral("title")).toString();
            manCid = c.value(QStringLiteral("cid")).toString();
            break;
        }
    }
    const QStringList candidates = LezClient::deriveIaCandidates(manCid, title);
    if (candidates.isEmpty() || candidates.first().endsWith(QLatin1Char('|'))) {
        m_lez->setCollectionState(collectionId, QStringLiteral("error"));
        m_lastError = QStringLiteral("not_on_network_and_no_ia_source");
        return;
    }
    if (!m_reseedCollectionId.isEmpty()) {
        m_lez->setCollectionState(collectionId, QStringLiteral("error"));
        m_lastError = QStringLiteral("reseed_busy");
        return;
    }
    m_reseedCollectionId = collectionId;
    m_reseedCid = cid;
    m_reseedCandidates = candidates;
    tryReseedCandidate(collectionId);
}

void ArchivePlugin::tryReseedCandidate(const QString& collectionId)
{
    const QStringList parts = m_reseedCandidates.takeFirst().split(QLatin1Char('|'));
    const QString iaId = parts.value(0);
    const QString iaFile = parts.value(1);

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    const QUrl url(QStringLiteral("https://archive.org/download/%1/%2").arg(iaId, iaFile));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "ia-basecamp/0.2");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);   // IA always redirects
    req.setTransferTimeout(30000);   // inactivity cap — dead IA dn-nodes hang forever
    qInfo() << "ArchivePlugin: reseed downloading" << url.toString();
    QNetworkReply* reply = m_nam->get(req);

    // EXACTLY keeper's temp naming: the storage dataset manifest embeds the
    // filename, so reproducing the inscribed CID requires the same name keeper
    // uploaded under — not just the same bytes
    const QString tmpPath = QDir::tempPath()
        + QStringLiteral("/keeper-%1-%2")
              .arg(QString(iaId).replace(QLatin1Char('/'), QLatin1Char('_')),
                   QString(iaFile).replace(QLatin1Char('/'), QLatin1Char('_')));
    auto* out = new QFile(tmpPath, reply);
    out->open(QIODevice::WriteOnly | QIODevice::Truncate);
    m_reseedTmpPath = tmpPath;

    connect(reply, &QNetworkReply::readyRead, this, [reply, out] {
        out->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, collectionId](qint64 recv, qint64 total) {
        const int pct = total > 0 ? int(recv * 100 / total) : 0;
        m_lez->setCollectionState(collectionId, QStringLiteral("mirroring"), pct);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, out, collectionId, tmpPath] {
        out->close();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(tmpPath);
            m_reseedTmpPath.clear();
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning() << "ArchivePlugin: reseed candidate failed for" << collectionId
                       << "http" << http << reply->errorString();
            // the keeper label split is ambiguous — try the next (id,file) boundary
            if (!m_reseedCandidates.isEmpty()) {
                tryReseedCandidate(collectionId);
                return;
            }
            m_reseedCollectionId.clear();
            const bool notAvailable = http == 404 || http == 503
                || reply->error() == QNetworkReply::ContentNotFoundError
                || reply->error() == QNetworkReply::OperationCanceledError;
            m_lez->setCollectionState(collectionId, QStringLiteral("error"));
            m_lastError = notAvailable
                ? QStringLiteral("file not available on the Internet Archive")
                : QStringLiteral("ia_download_failed: ") + reply->errorString();
            return;
        }
        qInfo() << "ArchivePlugin: reseed downloaded" << out->size() << "bytes — uploading";
        // hand the bytes to Logos Storage — completion via uploadFinished above
        m_storage->upload(tmpPath);
    });
}

QString ArchivePlugin::getCollections(const QString& channelId)
{
    if (!channelId.isEmpty() && !m_lez->isFollowed(channelId.toLower()))
        return fail(QStringLiteral("not_followed"));
    return ok({ { QStringLiteral("collections"), m_lez->collectionsJson(channelId) } });
}

QString ArchivePlugin::mirrorCollection(const QString& collectionId)
{
    const QString cid = m_lez->collectionCid(collectionId);
    if (cid.isEmpty())
        return fail(QStringLiteral("unknown_collection"));
    if (m_cidToCollection.contains(cid))
        return fail(QStringLiteral("storage_busy"));
    if (m_storage->storageState() != QLatin1String("ready"))
        return fail(m_storage->storageState() == QLatin1String("starting")
                        ? QStringLiteral("storage_starting")
                        : QStringLiteral("storage_offline"));
    m_cidToCollection.insert(cid, collectionId);
    m_lez->setCollectionState(collectionId, QStringLiteral("mirroring"), 0);
    m_storage->pin(cid);
    return ok({ { QStringLiteral("collectionId"), collectionId },
                { QStringLiteral("cid"), cid },
                { QStringLiteral("mode"), m_lez->preserveMode() } });
}

QString ArchivePlugin::unmirrorCollection(const QString& collectionId)
{
    const QString cid = m_lez->collectionCid(collectionId);
    if (cid.isEmpty())
        return fail(QStringLiteral("unknown_collection"));
    if (m_cidToCollection.contains(cid))
        return fail(QStringLiteral("storage_busy"));
    if (m_storage->storageState() != QLatin1String("ready"))
        return fail(m_storage->storageState() == QLatin1String("starting")
                        ? QStringLiteral("storage_starting")
                        : QStringLiteral("storage_offline"));
    m_cidToCollection.insert(cid, collectionId);
    m_storage->unpin(cid);
    return ok({ { QStringLiteral("collectionId"), collectionId } });
}

QString ArchivePlugin::getMirrorStatus(const QString& collectionId)
{
    const QString state = m_lez->collectionState(collectionId);
    if (state.isEmpty())
        return fail(QStringLiteral("unknown_collection"));
    return ok({ { QStringLiteral("collectionId"), collectionId },
                { QStringLiteral("state"), state },
                { QStringLiteral("storageState"), m_storage->storageState() } });
}

// ── share cards ─────────────────────────────────────────────────────────────

QString ArchivePlugin::cardsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
           + QStringLiteral("/ia-archive");
}

QString ArchivePlugin::getShareData(const QString& scope)
{
    const auto gws = m_lez->gateways();
    const QString thumbBase =
        gws.isEmpty() ? QString() : gws.at(m_lez->activeGateway()).storageUrl;
    const QJsonObject data =
        ShareHelper::buildShareData(m_lez->collectionsJson(), scope, thumbBase, m_usedBytes);
    return QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
}

QString ArchivePlugin::saveShareCard(const QString& pngBase64, const QString& name)
{
    const QJsonObject r = ShareHelper::savePngBase64(cardsDir(), name, pngBase64);
    if (!r.value(QStringLiteral("ok")).toBool())
        m_lastError = r.value(QStringLiteral("error")).toString();
    return QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
}

QString ArchivePlugin::finalizeShareCard(const QString& tmpPath, const QString& name)
{
    // grabToImage's saveToFile wrote the PNG (no Canvas leg — it silently never
    // paints while invisible); validate + move it into the cards dir
    const QJsonObject r = ShareHelper::saveFromFile(cardsDir(), name, tmpPath);
    if (!r.value(QStringLiteral("ok")).toBool())
        m_lastError = r.value(QStringLiteral("error")).toString();
    return QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
}

QString ArchivePlugin::openExplorerTx(const QString& txHash, const QString& blockHash,
                                      int txIndex)
{
    // The scan stores the node's mantle_tx.hash, which the explorer does not index
    // (#9) — the explorer's own hash is reachable only via the tx's position in its
    // block, so resolve through /api/blocks/{blockHash} and fall back to the block
    // page when the join isn't possible.
    static const QRegularExpression hex64(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (!hex64.match(txHash).hasMatch() && !hex64.match(blockHash).hasMatch())
        return fail(QStringLiteral("invalid_tx_hash"));

    const QString base = QString::fromLatin1(LezClient::kExplorerBase);
    QString url;

    if (hex64.match(blockHash).hasMatch()) {
        url = base + QStringLiteral("/blocks/") + blockHash.toLower();   // always-valid fallback
        if (txIndex >= 0) {
            if (!m_nam)
                m_nam = new QNetworkAccessManager(this);
            QNetworkRequest req(QUrl(base + QStringLiteral("/api/blocks/") + blockHash.toLower()));
            req.setTransferTimeout(5000);
            QNetworkReply* reply = m_nam->get(req);
            QEventLoop loop;
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonObject blk = QJsonDocument::fromJson(reply->readAll()).object();
                for (const QJsonValue& tv : blk.value(QLatin1String("transactions")).toArray()) {
                    const QJsonObject tx = tv.toObject();
                    if (tx.value(QLatin1String("index_in_block")).toInt(-1) == txIndex) {
                        const QString eh = tx.value(QLatin1String("tx_hash")).toString();
                        if (hex64.match(eh).hasMatch())
                            url = base + QStringLiteral("/txs/") + eh.toLower();
                        break;
                    }
                }
            }
            reply->deleteLater();
        }
    } else {
        // legacy rows without blockHash: the node hash is all we have — likely a
        // dead link on the explorer, but there is nothing better to offer
        url = base + QStringLiteral("/txs/") + txHash.toLower();
    }

#if defined(Q_OS_DARWIN)
    const QString opener = QStringLiteral("open");
#elif defined(Q_OS_WIN)
    const QString opener = QStringLiteral("explorer");
#else
    const QString opener = QStringLiteral("xdg-open");
#endif
    if (!QProcess::startDetached(opener, { url }))
        return fail(QStringLiteral("cannot_open_browser"));
    return ok({ { QStringLiteral("url"), url } });
}

QString ArchivePlugin::revealCard(const QString& path)
{
    // logos_host has no QGuiApplication → QDesktopServices is unavailable; open the
    // folder (never the file) with the platform opener. Scoped to the cards dir.
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    const QString dirCanonical = QFileInfo(cardsDir()).canonicalFilePath();
    if (canonical.isEmpty() || dirCanonical.isEmpty()
        || !canonical.startsWith(dirCanonical + QLatin1Char('/'))
        || fi.suffix() != QLatin1String("png"))
        return fail(QStringLiteral("no_such_card"));
#if defined(Q_OS_DARWIN)
    const QString opener = QStringLiteral("open");
#elif defined(Q_OS_WIN)
    const QString opener = QStringLiteral("explorer");
#else
    const QString opener = QStringLiteral("xdg-open");
#endif
    if (!QProcess::startDetached(opener, { dirCanonical }))
        return fail(QStringLiteral("cannot_open_folder"));
    return ok({ { QStringLiteral("path"), canonical } });
}
