#include "lez_client.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

namespace {
constexpr int kChannelInscribeOpcode = 17;

QString jsonStr(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
} // namespace

LezClient::LezClient(QObject* parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    // Dev default until campaign gateways exist (SPEC §4.1 / docs/spikes/p0-channel-read.md).
    m_gateways = { { QStringLiteral("http://100.108.127.3:8080"), QString() } };
}

// ── config ───────────────────────────────────────────────────────────────────

void LezClient::setGateways(const QList<Gateway>& gws)
{
    if (gws.isEmpty())
        return;
    m_gateways = gws;
    m_active = 0;
    saveState();
    // no implicit poll — callers decide when to probe (keeps request flow deterministic)
}

void LezClient::setPreserveMode(const QString& mode)
{
    m_preserveMode = mode;
    saveState();
}

// ── HTTP plumbing ────────────────────────────────────────────────────────────

QNetworkReply* LezClient::httpGet(const QString& path, const QString& query)
{
    const Gateway& gw = m_gateways.at(m_active);
    QUrl url(gw.nodeUrl);
    QString base = url.path();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    url.setPath(base + path);          // structured join — no raw string concatenation
    if (!query.isEmpty())
        url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(kHttpTimeoutMs);
    return m_net->get(req);
}

void LezClient::failOver(const QString& code)
{
    m_gatewayState = QStringLiteral("offline");
    emit healthChanged(m_gatewayState, m_syncLag);   // unconditional — initial state is also "offline"
    emit errorOccurred(code);
    if (m_gateways.size() > 1)
        m_active = (m_active + 1) % m_gateways.size();
}

// ── health ───────────────────────────────────────────────────────────────────

void LezClient::pollHealth()
{
    QNetworkReply* reply = httpGet(QStringLiteral("/cryptarchia/info"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            failOver(QStringLiteral("gateway_unreachable"));
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonObject info = body.size() <= 1024 * 1024
                                     ? QJsonDocument::fromJson(body).object()
                                     : QJsonObject{};
        if (!info.contains(QLatin1String("lib_slot"))) {
            failOver(QStringLiteral("gateway_bad_response"));
            return;
        }
        const qint64 slot = info.value(QLatin1String("slot")).toVariant().toLongLong();
        const qint64 lib  = info.value(QLatin1String("lib_slot")).toVariant().toLongLong();
        const QString mode = jsonStr(info, "mode");

        m_syncLag = qMax<qint64>(0, slot - lib);
        const bool healthy = (mode.isEmpty() || mode == QLatin1String("Online"))
                             && m_syncLag < kLagDegradedThreshold;
        m_gatewayState = healthy ? QStringLiteral("ready") : QStringLiteral("degraded");
        emit healthChanged(m_gatewayState, m_syncLag);
        // federation: a lagging gateway is demoted like a dead one — the next poll
        // (and the next scan) tries the next gateway in the failover order
        if (!healthy && m_gateways.size() > 1)
            m_active = (m_active + 1) % m_gateways.size();
    });
}

// ── channel ref parsing ──────────────────────────────────────────────────────

QString LezClient::parseChannelRef(const QString& ref, qint64* startSlot, QString* errorCode)
{
    if (startSlot)
        *startSlot = 0;
    // "@<startSlot>" only counts when anchored to the channel id itself —
    // an unrelated @123 elsewhere in a pasted URL must not become a slot hint
    static const QRegularExpression hexRe(QStringLiteral("([0-9a-fA-F]{64})(?:@(\\d+))?"));
    const QRegularExpressionMatch m = hexRe.match(ref);
    if (!m.hasMatch()) {
        if (errorCode)
            *errorCode = QStringLiteral("invalid_channel_ref");
        return {};
    }
    if (startSlot && !m.captured(2).isEmpty())
        *startSlot = m.captured(2).toLongLong();
    return m.captured(1).toLower();
}

// ── inscription decode ───────────────────────────────────────────────────────

QVector<LezClient::Collection> LezClient::extractCollections(const QJsonArray& blocks,
                                                             const QString& channelId,
                                                             qint64 libSlot)
{
    QVector<Collection> out;
    for (const QJsonValue& bv : blocks) {
        const QJsonObject block = bv.toObject();
        const QJsonObject header = block.value(QLatin1String("header")).toObject();
        const qint64 slot = header.value(QLatin1String("slot")).toVariant().toLongLong();
        if (slot > libSlot)
            continue;   // finalized data only — never surface pre-LIB inscriptions

        const QJsonArray txs = block.value(QLatin1String("transactions")).toArray();
        for (const QJsonValue& tv : txs) {
            const QJsonObject tx = tv.toObject();
            const QJsonObject mantle = tx.contains(QLatin1String("mantle_tx"))
                                           ? tx.value(QLatin1String("mantle_tx")).toObject()
                                           : tx;
            const QString txHash = jsonStr(mantle, "hash");
            const QJsonArray ops = mantle.value(QLatin1String("ops")).toArray();
            for (const QJsonValue& ov : ops) {
                const QJsonObject op = ov.toObject();
                if (op.value(QLatin1String("opcode")).toInt() != kChannelInscribeOpcode)
                    continue;
                const QJsonObject payload = op.value(QLatin1String("payload")).toObject();
                if (jsonStr(payload, "channel_id").toLower() != channelId)
                    continue;

                // inscription = byte array → UTF-8 JSON manifest (non-JSON payloads are
                // some other producer's data on this channel — skip, don't fail the scan)
                const QJsonArray arr = payload.value(QLatin1String("inscription")).toArray();
                if (arr.size() > kMaxInscriptionBytes)
                    continue;   // no legitimate manifest is this big — refuse the allocation
                QByteArray bytes;
                bytes.reserve(arr.size());
                for (const QJsonValue& byte : arr)
                    bytes.append(static_cast<char>(byte.toInt()));
                const QJsonDocument doc = QJsonDocument::fromJson(bytes);
                if (!doc.isObject())
                    continue;
                const QJsonObject man = doc.object();
                const QString cid = jsonStr(man, "cid");
                if (cid.isEmpty())
                    continue;   // a collection without content is not preservable

                Collection c;
                c.channelId = channelId;
                c.cid = cid;
                c.txHash = txHash;
                c.curator = jsonStr(payload, "signer");
                c.inscribedAtSlot = slot;
                c.id = jsonStr(man, "id").isEmpty() ? txHash : jsonStr(man, "id");
                c.title = jsonStr(man, "title").isEmpty() ? cid : jsonStr(man, "title");
                c.sizeBytes = man.value(QLatin1String("sizeBytes")).toVariant().toLongLong();
                c.items = man.value(QLatin1String("items")).toVariant().toLongLong();
                c.thumbnail = jsonStr(man, "thumbnail");
                c.state = QStringLiteral("available");
                out.append(c);
            }
        }
    }
    return out;
}

// ── follow / refresh ─────────────────────────────────────────────────────────

QString LezClient::followChannel(const QString& ref, QString* errorCode)
{
    qint64 startSlot = 0;
    QString err;
    const QString channelId = parseChannelRef(ref, &startSlot, &err);
    if (channelId.isEmpty()) {
        if (errorCode)
            *errorCode = err;
        return {};
    }
    if (m_channels.contains(channelId)) {
        if (errorCode)
            *errorCode = QStringLiteral("already_followed");
        return {};
    }
    Channel ch;
    ch.channelId = channelId;
    ch.startSlot = startSlot;
    ch.generation = ++m_generationCounter;
    m_channels.insert(channelId, ch);
    saveState();
    emit channelsChanged();
    startScan(channelId);
    return channelId;
}

bool LezClient::unfollowChannel(const QString& channelId)
{
    const QString id = channelId.toLower();
    if (m_channels.remove(id) == 0)
        return false;
    m_scanning.remove(id);   // orphan any in-flight scan; its callbacks self-discard
    saveState();
    emit channelsChanged();
    emit collectionsChanged();
    return true;
}

bool LezClient::refreshChannel(const QString& channelId)
{
    const QString id = channelId.toLower();
    if (!m_channels.contains(id) || m_scanning.contains(id))
        return false;
    startScan(id);
    return true;
}

void LezClient::startScan(const QString& channelId)
{
    if (m_scanning.contains(channelId))
        return;
    const qint64 generation = m_channels.value(channelId).generation;
    m_scanning.insert(channelId, generation);

    QNetworkReply* reply = httpGet(QStringLiteral("/cryptarchia/info"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, channelId, generation] {
        reply->deleteLater();
        if (m_scanning.value(channelId, -1) != generation)
            return;   // unfollowed (or re-followed) while in flight — not ours anymore
        if (reply->error() != QNetworkReply::NoError) {
            m_scanning.remove(channelId);
            failOver(QStringLiteral("gateway_unreachable"));
            emit scanFinished(channelId, false);
            return;
        }
        const QJsonObject info = QJsonDocument::fromJson(reply->readAll()).object();
        const qint64 libSlot = info.value(QLatin1String("lib_slot")).toVariant().toLongLong();
        if (libSlot <= 0) {
            m_scanning.remove(channelId);
            failOver(QStringLiteral("gateway_bad_response"));
            emit scanFinished(channelId, false);
            return;
        }
        scanNextPage(channelId, generation, libSlot, kMaxPagesPerRefresh);
    });
}

void LezClient::scanNextPage(const QString& channelId, qint64 generation, qint64 libSlot,
                             int pagesLeft)
{
    if (m_scanning.value(channelId, -1) != generation)
        return;
    Q_ASSERT(m_channels.contains(channelId));
    Channel& ch = m_channels[channelId];
    const qint64 from = ch.cursor > 0 ? ch.cursor + 1 : ch.startSlot;
    if (from > libSlot || pagesLeft <= 0) {
        ch.synced = (from > libSlot);
        m_scanning.remove(channelId);
        saveState();
        emit channelsChanged();
        emit scanFinished(channelId, ch.synced);
        return;
    }
    const qint64 to = qMin(from + kPageSlots - 1, libSlot);

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("slot_from"), QString::number(from));
    q.addQueryItem(QStringLiteral("slot_to"), QString::number(to));
    QNetworkReply* reply = httpGet(QStringLiteral("/cryptarchia/blocks"), q.toString());
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, generation, libSlot, pagesLeft, to] {
        reply->deleteLater();
        if (m_scanning.value(channelId, -1) != generation)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            m_scanning.remove(channelId);
            saveState();   // keep the cursor we reached
            failOver(QStringLiteral("gateway_unreachable"));
            emit scanFinished(channelId, false);
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.size() > kMaxBlocksBodyBytes) {
            m_scanning.remove(channelId);
            saveState();
            failOver(QStringLiteral("gateway_bad_response"));
            emit scanFinished(channelId, false);
            return;
        }
        const QJsonArray blocks = QJsonDocument::fromJson(body).array();
        const QVector<Collection> found = extractCollections(blocks, channelId, libSlot);

        // reacquire after the parse — never hold the reference across anything async
        Channel& ch = m_channels[channelId];
        bool added = false;
        for (const Collection& c : found) {
            const bool dup = std::any_of(ch.collections.cbegin(), ch.collections.cend(),
                                         [&c](const Collection& e) { return e.txHash == c.txHash && e.id == c.id; });
            if (dup)
                continue;
            ch.collections.append(c);
            ch.lastInscription = qMax(ch.lastInscription, c.inscribedAtSlot);
            ch.curator = c.curator;
            added = true;
        }
        ch.cursor = to;
        if (added)
            emit collectionsChanged();
        scanNextPage(channelId, generation, libSlot, pagesLeft - 1);
    });
}

// ── JSON views ───────────────────────────────────────────────────────────────

QJsonArray LezClient::channelsJson() const
{
    QJsonArray arr;
    for (const Channel& ch : m_channels) {
        arr.append(QJsonObject{
            { QStringLiteral("channelId"), ch.channelId },
            { QStringLiteral("name"), ch.channelId.left(8) },
            { QStringLiteral("curator"), ch.curator },
            { QStringLiteral("collections"), ch.collections.size() },
            { QStringLiteral("lastInscription"), ch.lastInscription },
            { QStringLiteral("synced"), ch.synced },
        });
    }
    return arr;
}

QJsonArray LezClient::collectionsJson(const QString& channelId) const
{
    QJsonArray arr;
    for (const Channel& ch : m_channels) {
        if (!channelId.isEmpty() && ch.channelId != channelId.toLower())
            continue;
        for (const Collection& c : ch.collections) {
            arr.append(QJsonObject{
                { QStringLiteral("id"), c.id },
                { QStringLiteral("title"), c.title },
                { QStringLiteral("channelId"), c.channelId },
                { QStringLiteral("cid"), c.cid },
                { QStringLiteral("sizeBytes"), c.sizeBytes },
                { QStringLiteral("items"), c.items },
                { QStringLiteral("thumbnail"), c.thumbnail },
                { QStringLiteral("inscribedAt"), c.inscribedAtSlot },
                { QStringLiteral("txHash"), c.txHash },
                { QStringLiteral("curator"), c.curator },
                { QStringLiteral("state"), c.state },
                { QStringLiteral("progressBlocks"), c.progressBlocks },
            });
        }
    }
    return arr;
}

QJsonObject LezClient::summaryJson() const
{
    qint64 collections = 0;
    qint64 mirrored = 0;
    for (const Channel& ch : m_channels) {
        collections += ch.collections.size();
        for (const Collection& c : ch.collections)
            if (c.state == QLatin1String("mirrored"))
                ++mirrored;
    }
    return QJsonObject{
        { QStringLiteral("following"), m_channels.size() },
        { QStringLiteral("collections"), collections },
        { QStringLiteral("mirrored"), mirrored },
        { QStringLiteral("usedBytes"), 0 },    // overridden by the plugin from repo/stat
    };
}

QString LezClient::collectionCid(const QString& collectionId) const
{
    for (const Channel& ch : m_channels)
        for (const Collection& c : ch.collections)
            if (c.id == collectionId)
                return c.cid;
    return {};
}

QString LezClient::collectionState(const QString& collectionId) const
{
    for (const Channel& ch : m_channels)
        for (const Collection& c : ch.collections)
            if (c.id == collectionId)
                return c.state;
    return {};
}

bool LezClient::setCollectionState(const QString& collectionId, const QString& state,
                                   qint64 progressBlocks)
{
    for (Channel& ch : m_channels) {
        for (Collection& c : ch.collections) {
            if (c.id != collectionId)
                continue;
            c.state = state;
            if (progressBlocks >= 0)
                c.progressBlocks = progressBlocks;
            if (state != QLatin1String("mirroring")) {
                c.progressBlocks = 0;
                saveState();   // terminal states only — progress ticks stay off disk
            }
            emit collectionsChanged();
            return true;
        }
    }
    return false;
}

// ── persistence ──────────────────────────────────────────────────────────────

QString LezClient::stateFilePath()
{
    // GenericDataLocation honors XDG_DATA_HOME → tests isolate with a temp dir
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/ia-archive");
    return dir + QStringLiteral("/state.json");
}

void LezClient::saveState() const
{
    QJsonArray gws;
    for (const Gateway& g : m_gateways)
        gws.append(QJsonObject{ { QStringLiteral("nodeUrl"), g.nodeUrl },
                                { QStringLiteral("storageUrl"), g.storageUrl } });

    QJsonArray chans;
    for (const Channel& ch : m_channels) {
        QJsonArray cols;
        for (const Collection& c : ch.collections) {
            cols.append(QJsonObject{
                { QStringLiteral("id"), c.id },
                { QStringLiteral("title"), c.title },
                { QStringLiteral("cid"), c.cid },
                { QStringLiteral("sizeBytes"), c.sizeBytes },
                { QStringLiteral("items"), c.items },
                { QStringLiteral("thumbnail"), c.thumbnail },
                { QStringLiteral("inscribedAt"), c.inscribedAtSlot },
                { QStringLiteral("txHash"), c.txHash },
                { QStringLiteral("curator"), c.curator },
                { QStringLiteral("state"), c.state },
            });
        }
        chans.append(QJsonObject{
            { QStringLiteral("channelId"), ch.channelId },
            { QStringLiteral("startSlot"), ch.startSlot },
            { QStringLiteral("cursor"), ch.cursor },
            { QStringLiteral("lastInscription"), ch.lastInscription },
            { QStringLiteral("curator"), ch.curator },
            { QStringLiteral("synced"), ch.synced },
            { QStringLiteral("collections"), cols },
        });
    }

    const QJsonObject root{
        { QStringLiteral("gateways"), gws },
        { QStringLiteral("preserveMode"), m_preserveMode },
        { QStringLiteral("channels"), chans },
    };

    QDir().mkpath(QFileInfo(stateFilePath()).path());
    QSaveFile f(stateFilePath());   // atomic: write-to-temp + rename, no torn states
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void LezClient::loadState()
{
    QFile f(stateFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        // corrupt state is preserved for inspection, never silently discarded
        f.close();
        QFile::remove(stateFilePath() + QStringLiteral(".corrupt"));
        QFile::rename(stateFilePath(), stateFilePath() + QStringLiteral(".corrupt"));
        emit errorOccurred(QStringLiteral("state_corrupt"));
        return;
    }
    const QJsonObject root = doc.object();

    QList<Gateway> gws;
    for (const QJsonValue& gv : root.value(QLatin1String("gateways")).toArray()) {
        const QJsonObject g = gv.toObject();
        gws.append({ jsonStr(g, "nodeUrl"), jsonStr(g, "storageUrl") });
    }
    if (!gws.isEmpty()) {
        m_gateways = gws;
        m_active = 0;
    }
    const QString mode = jsonStr(root, "preserveMode");
    if (!mode.isEmpty())
        m_preserveMode = mode;

    m_channels.clear();
    for (const QJsonValue& cv : root.value(QLatin1String("channels")).toArray()) {
        const QJsonObject co = cv.toObject();
        Channel ch;
        ch.channelId = jsonStr(co, "channelId");
        ch.startSlot = co.value(QLatin1String("startSlot")).toVariant().toLongLong();
        ch.cursor = co.value(QLatin1String("cursor")).toVariant().toLongLong();
        ch.lastInscription = co.value(QLatin1String("lastInscription")).toVariant().toLongLong();
        ch.curator = jsonStr(co, "curator");
        ch.synced = co.value(QLatin1String("synced")).toBool();
        for (const QJsonValue& colv : co.value(QLatin1String("collections")).toArray()) {
            const QJsonObject c = colv.toObject();
            Collection col;
            col.id = jsonStr(c, "id");
            col.title = jsonStr(c, "title");
            col.channelId = ch.channelId;
            col.cid = jsonStr(c, "cid");
            col.sizeBytes = c.value(QLatin1String("sizeBytes")).toVariant().toLongLong();
            col.items = c.value(QLatin1String("items")).toVariant().toLongLong();
            col.thumbnail = jsonStr(c, "thumbnail");
            col.inscribedAtSlot = c.value(QLatin1String("inscribedAt")).toVariant().toLongLong();
            col.txHash = jsonStr(c, "txHash");
            col.curator = jsonStr(c, "curator");
            col.state = jsonStr(c, "state");
            if (col.state == QLatin1String("mirroring"))   // no pin survives a restart
                col.state = QStringLiteral("available");
            ch.collections.append(col);
        }
        if (!ch.channelId.isEmpty())
            m_channels.insert(ch.channelId, ch);
    }
    if (!m_channels.isEmpty()) {
        emit channelsChanged();
        emit collectionsChanged();
    }
}
