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

QNetworkReply* LezClient::httpGet(const QString& path, const QString& query, int gatewayIdx)
{
    const int idx = (gatewayIdx >= 0 && gatewayIdx < m_gateways.size()) ? gatewayIdx : m_active;
    const Gateway& gw = m_gateways.at(idx);
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

QNetworkReply* LezClient::httpGetUrl(const QUrl& url)
{
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
        const QJsonObject root = body.size() <= 1024 * 1024
                                     ? QJsonDocument::fromJson(body).object()
                                     : QJsonObject{};
        // v0.2 nests slot/lib_slot under "cryptarchia_info" (systemic rename); older nodes had them at root.
        const QJsonObject info = root.contains(QLatin1String("cryptarchia_info"))
                                     ? root.value(QLatin1String("cryptarchia_info")).toObject()
                                     : root;
        if (!info.contains(QLatin1String("lib_slot"))) {
            failOver(QStringLiteral("gateway_bad_response"));
            return;
        }
        const qint64 slot = info.value(QLatin1String("slot")).toVariant().toLongLong();
        const qint64 lib  = info.value(QLatin1String("lib_slot")).toVariant().toLongLong();
        // mode is {"Started":"Online"} in v0.2 (object) or a plain "Online" string in older nodes.
        const QJsonValue modeVal = root.value(QLatin1String("mode"));
        const QString mode = modeVal.isString()
                                 ? modeVal.toString()
                                 : (modeVal.isObject() && !modeVal.toObject().isEmpty()
                                        ? modeVal.toObject().begin().value().toString()
                                        : QString());

        // slot - lib is the protocol FINALIZATION DEPTH (normally thousands of slots), NOT sync lag —
        // surface it for info, but don't gate readiness on it (that pinned a healthy node to "degraded"
        // forever: real testnet depth ~3.5k ≫ the old 1200 threshold). Ready = reachable + Online.
        m_syncLag = qMax<qint64>(0, slot - lib);
        const bool healthy = (mode.isEmpty() || mode == QLatin1String("Online"));
        m_gatewayState = healthy ? QStringLiteral("ready") : QStringLiteral("degraded");
        emit healthChanged(m_gatewayState, m_syncLag);
        // a degraded (lagging) gateway is surfaced, not rotated away from: with one
        // real gateway the old demote-and-rotate state machine was pure debugging
        // surface (#11) — dead gateways still fail over in failOver()
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

QVector<LezClient::Item> LezClient::extractItems(const QJsonArray& blocks,
                                                             const QString& channelId,
                                                             qint64 libSlot,
                                                             ScanStats* stats)
{
    // every skip below is counted in *stats — a defensive skip that fails
    // silently costs more debugging time than it saves (#11)
    ScanStats local;
    ScanStats& st = stats ? *stats : local;

    QVector<Item> out;
    for (const QJsonValue& bv : blocks) {
        const QJsonObject block = bv.toObject();
        const QJsonObject header = block.value(QLatin1String("header")).toObject();
        const qint64 slot = header.value(QLatin1String("slot")).toVariant().toLongLong();
        if (slot > libSlot) {
            st.skippedNotFinalized++;
            continue;   // finalized data only — never surface pre-LIB inscriptions
        }
        const QString blockHash = jsonStr(header, "id");

        const QJsonArray txs = block.value(QLatin1String("transactions")).toArray();
        for (int txIdx = 0; txIdx < txs.size(); ++txIdx) {
            const QJsonObject tx = txs.at(txIdx).toObject();
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
                if (jsonStr(payload, "channel_id").toLower() != channelId) {
                    st.otherChannelOps++;
                    continue;
                }

                // inscription = byte array → UTF-8 JSON manifest (non-JSON payloads are
                // some other producer's data on this channel — skip, don't fail the scan)
                const QJsonArray arr = payload.value(QLatin1String("inscription")).toArray();
                if (arr.size() > kMaxInscriptionBytes) {
                    st.skippedOversized++;
                    continue;   // no legitimate manifest is this big — refuse the allocation
                }
                QByteArray bytes;
                bytes.reserve(arr.size());
                for (const QJsonValue& byte : arr)
                    bytes.append(static_cast<char>(byte.toInt()));
                const QJsonDocument doc = QJsonDocument::fromJson(bytes);
                if (!doc.isObject()) {
                    st.skippedNonJson++;
                    continue;
                }
                const QJsonObject man = doc.object();
                const QString cid = jsonStr(man, "cid");
                const bool iaEntry = jsonStr(man, "type") == QLatin1String("ia_item");
                if (cid.isEmpty() && !iaEntry) {
                    st.skippedNoCid++;
                    continue;   // an item without content is not preservable
                }

                Item c;
                c.channelId = channelId;
                c.txHash = txHash;
                c.blockHash = blockHash;
                c.txIndex = txIdx;
                c.curator = jsonStr(payload, "signer");
                c.inscribedAtSlot = slot;
                c.state = QStringLiteral("available");

                if (iaEntry) {
                    // Campaign payload v2 (#14, docs/campaign-brief.md): the entry names
                    // an IA item — no CID; content comes from archive.org, verified
                    // against IA's own per-file checksums at preserve time.
                    const QString iaId = jsonStr(man, "id");
                    if (iaId.isEmpty()) {
                        st.skippedNoCid++;   // an ia_item without an id is not preservable
                        continue;
                    }
                    c.iaId = iaId;
                    c.id = iaId;
                    c.title = !jsonStr(man, "name").isEmpty() ? jsonStr(man, "name") : iaId;
                    c.sizeBytes = man.value(QLatin1String("size")).toVariant().toLongLong();
                } else {
                    c.cid = cid;
                    // keeper cid_pin carries metadata as a NESTED JSON string in "label" —
                    // parse it and prefer the standard schema fields (keeper#43): name / content /
                    // image / totalSize, with legacy fallbacks (title / files / thumbnail).
                    const QString labelStr = jsonStr(man, "label");
                    QJsonObject lab;
                    if (labelStr.startsWith(QLatin1Char('{')))
                        lab = QJsonDocument::fromJson(labelStr.toUtf8()).object();
                    c.id = !jsonStr(man, "id").isEmpty() ? jsonStr(man, "id")
                         : !jsonStr(lab, "id").isEmpty() ? jsonStr(lab, "id") : txHash;
                    c.title = !jsonStr(lab, "name").isEmpty()  ? jsonStr(lab, "name")
                            : !jsonStr(lab, "title").isEmpty() ? jsonStr(lab, "title")
                            : !jsonStr(man, "title").isEmpty() ? jsonStr(man, "title")
                            : !labelStr.isEmpty()              ? labelStr.left(120)
                                                               : cid;
                    c.sizeBytes = man.value(QLatin1String("sizeBytes")).toVariant().toLongLong();
                    if (c.sizeBytes == 0)
                        c.sizeBytes = lab.value(QLatin1String("totalSize")).toVariant().toLongLong();
                    const QJsonArray files = !lab.value(QLatin1String("content")).toArray().isEmpty()
                                                 ? lab.value(QLatin1String("content")).toArray()
                                                 : lab.value(QLatin1String("files")).toArray();
                    c.items = files.isEmpty()
                                  ? man.value(QLatin1String("items")).toVariant().toLongLong()
                                  : files.size();
                    c.thumbnail = !jsonStr(lab, "image").isEmpty() ? jsonStr(lab, "image")
                                                                   : jsonStr(man, "thumbnail");
                    deriveIaRef(c.cid, labelStr, &c.iaId, &c.iaFile);
                    if (c.iaId.isEmpty() && !jsonStr(lab, "id").isEmpty())
                        c.iaId = jsonStr(lab, "id");
                }
                st.matched++;
                out.append(c);
            }
        }
    }
    return out;
}

// keeper's cid_pin conventions carry the IA reference two ways:
//   cid:   "ia:<identifier>"                          → whole item
//   label: "Logos Storage: keeper-<id>-<file> → …"    → one file of an item
// (<id> may contain '-'; the filename is the part from the last '-' before
//  the first dotted segment)
void LezClient::deriveIaRef(const QString& cid, const QString& label,
                            QString* iaId, QString* iaFile)
{
    iaId->clear();
    iaFile->clear();
    if (cid.startsWith(QLatin1String("ia:"))) {
        *iaId = cid.mid(3);
        return;
    }
    static const QRegularExpression prefixRe(
        QStringLiteral("^Logos Storage: keeper-(.+?)(?:\\s*\\x{2192}.*)?$"));
    const QRegularExpressionMatch m = prefixRe.match(label);
    if (!m.hasMatch())
        return;
    const QString rest = m.captured(1).trimmed();   // "<id>-<file>"
    const int dot = rest.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        *iaId = rest;
        return;
    }
    const int split = rest.lastIndexOf(QLatin1Char('-'), dot);
    if (split <= 0) {
        *iaId = rest;
        return;
    }
    *iaId = rest.left(split);
    *iaFile = rest.mid(split + 1);
}

QStringList LezClient::deriveIaCandidates(const QString& cid, const QString& label)
{
    QStringList out;
    if (cid.startsWith(QLatin1String("ia:"))) {
        out << cid.mid(3) + QStringLiteral("|");
        return out;
    }
    static const QRegularExpression prefixRe(
        QStringLiteral("^Logos Storage: keeper-(.+?)(?:\\s*\\x{2192}.*)?$"));
    const QRegularExpressionMatch m = prefixRe.match(label);
    if (!m.hasMatch())
        return out;
    const QString rest = m.captured(1).trimmed();
    const int dot = rest.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        out << rest + QStringLiteral("|");
        return out;
    }
    // every '-' before the first dot is a possible id/file boundary; nearest-to-dot
    // first (the original heuristic), then walking leftward
    for (int pos = rest.lastIndexOf(QLatin1Char('-'), dot); pos > 0;
         pos = rest.lastIndexOf(QLatin1Char('-'), pos - 1)) {
        out << rest.left(pos) + QStringLiteral("|") + rest.mid(pos + 1);
        if (out.size() >= 4)
            break;   // archive.org hammering cap
    }
    if (out.isEmpty())
        out << rest + QStringLiteral("|");
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
    m_channels.insert(channelId, ch);
    saveState();
    emit channelsChanged();
    startScan(channelId);
    return channelId;
}

bool LezClient::setChannelLabel(const QString& channelId, const QString& label)
{
    const QString id = channelId.toLower();
    if (!m_channels.contains(id))
        return false;
    m_channels[id].label = label.left(64);
    saveState();
    emit channelsChanged();
    return true;
}

bool LezClient::setAutoPreserve(const QString& channelId, bool on)
{
    const QString id = channelId.toLower();
    if (!m_channels.contains(id))
        return false;
    m_channels[id].autoPreserve = on;
    saveState();
    emit channelsChanged();
    return true;
}

bool LezClient::unfollowChannel(const QString& channelId)
{
    const QString id = channelId.toLower();
    if (m_channels.remove(id) == 0)
        return false;
    delete m_scanning.take(id);   // kills the scan context → aborts in-flight replies,
                                  // disconnects every pending callback (#11)
    saveState();
    emit channelsChanged();
    emit itemsChanged();
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
    // the context object IS the scan's lifetime: every callback targets it and
    // every reply is parented to it — deleting it cancels the scan wholesale
    auto* ctx = new QObject(this);
    m_scanning.insert(channelId, ctx);
    m_channels[channelId].lastScan = ScanStats{};

    // pin the whole scan to one gateway: lib_slot and every page must come from the
    // same finalized view, or cursor advancement could silently skip inscriptions
    const int gatewayIdx = m_active;

    // #10: a fresh follow without a user @slot hint would crawl from genesis —
    // ask the explorer where the channel actually starts first
    const Channel& ch = m_channels.value(channelId);
    if (ch.startSlot == 0 && ch.cursor == 0 && !m_explorerBase.isEmpty())
        resolveScanStart(channelId, ctx, gatewayIdx);
    else
        fetchInfoAndScan(channelId, ctx, gatewayIdx);
}

void LezClient::endScan(const QString& channelId, QObject* ctx)
{
    m_scanning.remove(channelId);
    ctx->deleteLater();   // we may be inside a signal of a ctx-owned reply
}

void LezClient::resolveScanStart(const QString& channelId, QObject* ctx, int gatewayIdx)
{
    QNetworkReply* chReply = httpGetUrl(
        QUrl(m_explorerBase + QStringLiteral("/api/channel/") + channelId));
    chReply->setParent(ctx);
    connect(chReply, &QNetworkReply::finished, ctx, [this, chReply, channelId, ctx, gatewayIdx] {
        chReply->deleteLater();
        QString firstBlock;
        if (chReply->error() == QNetworkReply::NoError) {
            const QJsonObject info = QJsonDocument::fromJson(chReply->readAll()).object();
            firstBlock = info.value(QLatin1String("first_seen_block_id")).toString();
            // untrusted response lands in a URL — accept nothing but a block id
            static const QRegularExpression hex64(QStringLiteral("^[0-9a-fA-F]{64}$"));
            if (!hex64.match(firstBlock).hasMatch())
                firstBlock.clear();
        }
        if (firstBlock.isEmpty()) {   // explorer down or channel unknown — scan as before
            fetchInfoAndScan(channelId, ctx, gatewayIdx);
            return;
        }
        QNetworkReply* bReply = httpGetUrl(
            QUrl(m_explorerBase + QStringLiteral("/api/blocks/") + firstBlock));
        bReply->setParent(ctx);
        connect(bReply, &QNetworkReply::finished, ctx, [this, bReply, channelId, ctx, gatewayIdx] {
            bReply->deleteLater();
            if (bReply->error() == QNetworkReply::NoError) {
                const QJsonObject blk = QJsonDocument::fromJson(bReply->readAll()).object();
                const qint64 slot = blk.value(QLatin1String("slot")).toVariant().toLongLong();
                if (slot > 0 && m_channels.contains(channelId)) {
                    m_channels[channelId].startSlot = slot;   // persists — once per follow
                    saveState();
                }
            }
            fetchInfoAndScan(channelId, ctx, gatewayIdx);
        });
    });
}

void LezClient::fetchInfoAndScan(const QString& channelId, QObject* ctx, int gatewayIdx)
{
    QNetworkReply* reply = httpGet(QStringLiteral("/cryptarchia/info"), QString(), gatewayIdx);
    reply->setParent(ctx);
    connect(reply, &QNetworkReply::finished, ctx, [this, reply, channelId, ctx, gatewayIdx] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            endScan(channelId, ctx);
            failOver(QStringLiteral("gateway_unreachable"));
            emit scanFinished(channelId, false);
            return;
        }
        const QJsonObject info = QJsonDocument::fromJson(reply->readAll()).object();
        const qint64 libSlot = info.value(QLatin1String("lib_slot")).toVariant().toLongLong();
        if (libSlot <= 0) {
            endScan(channelId, ctx);
            failOver(QStringLiteral("gateway_bad_response"));
            emit scanFinished(channelId, false);
            return;
        }
        scanNextPage(channelId, ctx, gatewayIdx, libSlot, kMaxPagesPerRefresh);
    });
}

void LezClient::scanNextPage(const QString& channelId, QObject* ctx, int gatewayIdx,
                             qint64 libSlot, int pagesLeft)
{
    Q_ASSERT(m_channels.contains(channelId));
    Channel& ch = m_channels[channelId];
    ch.lastLibSlot = libSlot;
    const qint64 from = ch.cursor > 0 ? ch.cursor + 1 : ch.startSlot;
    if (from > libSlot || pagesLeft <= 0) {
        ch.synced = (from > libSlot);
        endScan(channelId, ctx);
        saveState();
        emit channelsChanged();
        emit scanFinished(channelId, ch.synced);
        return;
    }
    const qint64 to = qMin(from + kPageSlots - 1, libSlot);

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("slot_from"), QString::number(from));
    q.addQueryItem(QStringLiteral("slot_to"), QString::number(to));
    QNetworkReply* reply = httpGet(QStringLiteral("/cryptarchia/blocks"), q.toString(), gatewayIdx);
    reply->setParent(ctx);
    connect(reply, &QNetworkReply::finished, ctx,
            [this, reply, channelId, ctx, gatewayIdx, libSlot, pagesLeft, from, to] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            endScan(channelId, ctx);
            saveState();   // keep the cursor we reached
            failOver(QStringLiteral("gateway_unreachable"));
            emit scanFinished(channelId, false);
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.size() > kMaxBlocksBodyBytes) {
            m_channels[channelId].lastScan.oversizedBodies++;
            endScan(channelId, ctx);
            saveState();
            failOver(QStringLiteral("gateway_bad_response"));
            emit scanFinished(channelId, false);
            return;
        }
        const QJsonArray blocks = QJsonDocument::fromJson(body).array();
        ScanStats pageStats;
        const QVector<Item> found = extractItems(blocks, channelId, libSlot,
                                                             &pageStats);

        // reacquire after the parse — never hold the reference across anything async
        Channel& ch = m_channels[channelId];
        bool added = false;
        QStringList newIds;
        for (const Item& c : found) {
            // One row per item id. For ia_items the id is the IA identifier — the
            // only load-bearing field (docs/campaign-brief.md) — so re-inscribing
            // the same item is the same content, not a second row. Keeping id the
            // sole key also keeps it consistent with the bare-id lookups below
            // (itemCid/itemState/setItemState), which match the first id and stop.
            const bool dup = std::any_of(ch.items.cbegin(), ch.items.cend(),
                                         [&c](const Item& e) { return e.id == c.id; });
            if (dup) {
                pageStats.skippedDuplicate++;
                pageStats.matched--;
                continue;
            }
            ch.items.append(c);
            ch.lastInscription = qMax(ch.lastInscription, c.inscribedAtSlot);
            ch.curator = c.curator;
            newIds.append(c.id);
            added = true;
        }
        ch.cursor = to;

        ScanStats& total = ch.lastScan;
        total.scannedSlots += to - from + 1;
        total.pages++;
        total.matched += pageStats.matched;
        total.skippedNotFinalized += pageStats.skippedNotFinalized;
        total.skippedNonJson += pageStats.skippedNonJson;
        total.skippedOversized += pageStats.skippedOversized;
        total.skippedNoCid += pageStats.skippedNoCid;
        total.skippedDuplicate += pageStats.skippedDuplicate;
        total.otherChannelOps += pageStats.otherChannelOps;

        if (added) {
            emit itemsChanged();
            emit itemsDiscovered(channelId, newIds);
        }
        scanNextPage(channelId, ctx, gatewayIdx, libSlot, pagesLeft - 1);
    });
}

// ── JSON views ───────────────────────────────────────────────────────────────

QJsonArray LezClient::channelsJson() const
{
    QJsonArray arr;
    for (const Channel& ch : m_channels) {
        QJsonObject row{
            { QStringLiteral("channelId"), ch.channelId },
            { QStringLiteral("name"), ch.label.isEmpty() ? ch.channelId.left(8) : ch.label },
            { QStringLiteral("curator"), ch.curator },
            { QStringLiteral("items"), ch.items.size() },
            { QStringLiteral("lastInscription"), ch.lastInscription },
            { QStringLiteral("synced"), ch.synced },
            { QStringLiteral("autoPreserve"), ch.autoPreserve },
        };
        if (!ch.synced && ch.lastLibSlot > ch.startSlot && ch.cursor >= ch.startSlot)
            row.insert(QStringLiteral("progress"),
                       qBound<int>(0, int(100.0 * double(ch.cursor - ch.startSlot)
                                              / double(ch.lastLibSlot - ch.startSlot)), 100));
        arr.append(row);
    }
    return arr;
}

QJsonObject LezClient::scanDiagnosticsJson(const QString& channelId) const
{
    const QString id = channelId.toLower();
    if (!m_channels.contains(id))
        return QJsonObject{ { QStringLiteral("known"), false } };
    const ScanStats& s = m_channels.value(id).lastScan;
    return QJsonObject{
        { QStringLiteral("known"), true },
        { QStringLiteral("scannedSlots"), s.scannedSlots },
        { QStringLiteral("pages"), s.pages },
        { QStringLiteral("matched"), s.matched },
        { QStringLiteral("skippedNotFinalized"), s.skippedNotFinalized },
        { QStringLiteral("skippedNonJson"), s.skippedNonJson },
        { QStringLiteral("skippedOversized"), s.skippedOversized },
        { QStringLiteral("skippedNoCid"), s.skippedNoCid },
        { QStringLiteral("skippedDuplicate"), s.skippedDuplicate },
        { QStringLiteral("otherChannelOps"), s.otherChannelOps },
        { QStringLiteral("oversizedBodies"), s.oversizedBodies },
    };
}

QJsonArray LezClient::itemsJson(const QString& channelId) const
{
    QJsonArray arr;
    for (const Channel& ch : m_channels) {
        if (!channelId.isEmpty() && ch.channelId != channelId.toLower())
            continue;
        for (const Item& c : ch.items) {
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
                { QStringLiteral("blockHash"), c.blockHash },
                { QStringLiteral("txIndex"), c.txIndex },
                { QStringLiteral("curator"), c.curator },
                { QStringLiteral("state"), c.state },
                { QStringLiteral("progressBlocks"), c.progressBlocks },
                { QStringLiteral("iaId"), c.iaId },
                { QStringLiteral("iaFile"), c.iaFile },
            });
        }
    }
    return arr;
}

QJsonObject LezClient::summaryJson() const
{
    qint64 items = 0;
    qint64 mirrored = 0;
    for (const Channel& ch : m_channels) {
        items += ch.items.size();
        for (const Item& c : ch.items)
            if (c.state == QLatin1String("mirrored"))
                ++mirrored;
    }
    return QJsonObject{
        { QStringLiteral("following"), m_channels.size() },
        { QStringLiteral("items"), items },
        { QStringLiteral("mirrored"), mirrored },
        { QStringLiteral("usedBytes"), 0 },    // overridden by the plugin from repo/stat
    };
}

QString LezClient::itemCid(const QString& itemId) const
{
    for (const Channel& ch : m_channels)
        for (const Item& c : ch.items)
            if (c.id == itemId)
                return c.cid;
    return {};
}

QStringList LezClient::itemStoredCids(const QString& itemId) const
{
    for (const Channel& ch : m_channels)
        for (const Item& c : ch.items)
            if (c.id == itemId)
                return c.storedCids;
    return {};
}

bool LezClient::setItemStoredCids(const QString& itemId, const QStringList& cids)
{
    for (Channel& ch : m_channels) {
        for (Item& c : ch.items) {
            if (c.id != itemId)
                continue;
            c.storedCids = cids;
            saveState();
            return true;
        }
    }
    return false;
}

QString LezClient::itemState(const QString& itemId) const
{
    for (const Channel& ch : m_channels)
        for (const Item& c : ch.items)
            if (c.id == itemId)
                return c.state;
    return {};
}

bool LezClient::setItemState(const QString& itemId, const QString& state,
                                   qint64 progressBlocks)
{
    for (Channel& ch : m_channels) {
        for (Item& c : ch.items) {
            if (c.id != itemId)
                continue;
            c.state = state;
            if (progressBlocks >= 0)
                c.progressBlocks = progressBlocks;
            if (state != QLatin1String("mirroring")) {
                c.progressBlocks = 0;
                saveState();   // terminal states only — progress ticks stay off disk
            }
            emit itemsChanged();
            return true;
        }
    }
    return false;
}

bool LezClient::removeItem(const QString& itemId)
{
    // #29: drop a single item from its channel's list. It is on-chain, so a future
    // rescan can resurface it — a "forget from my view" action, not an unpreserve
    // (ia_item per-file unpin is unsupported in v1).
    for (Channel& ch : m_channels) {
        for (int i = 0; i < ch.items.size(); ++i) {
            if (ch.items[i].id != itemId)
                continue;
            ch.items.remove(i);
            saveState();
            emit itemsChanged();
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
        for (const Item& c : ch.items) {
            cols.append(QJsonObject{
                { QStringLiteral("id"), c.id },
                { QStringLiteral("title"), c.title },
                { QStringLiteral("cid"), c.cid },
                { QStringLiteral("sizeBytes"), c.sizeBytes },
                { QStringLiteral("items"), c.items },
                { QStringLiteral("thumbnail"), c.thumbnail },
                { QStringLiteral("inscribedAt"), c.inscribedAtSlot },
                { QStringLiteral("txHash"), c.txHash },
                { QStringLiteral("blockHash"), c.blockHash },
                { QStringLiteral("txIndex"), c.txIndex },
                { QStringLiteral("curator"), c.curator },
                { QStringLiteral("state"), c.state },
                { QStringLiteral("iaId"), c.iaId },
                { QStringLiteral("iaFile"), c.iaFile },
                { QStringLiteral("storedCids"), QJsonArray::fromStringList(c.storedCids) },
            });
        }
        chans.append(QJsonObject{
            { QStringLiteral("channelId"), ch.channelId },
            { QStringLiteral("label"), ch.label },
            { QStringLiteral("startSlot"), ch.startSlot },
            { QStringLiteral("cursor"), ch.cursor },
            { QStringLiteral("autoPreserve"), ch.autoPreserve },
            { QStringLiteral("lastInscription"), ch.lastInscription },
            { QStringLiteral("curator"), ch.curator },
            { QStringLiteral("synced"), ch.synced },
            { QStringLiteral("items"), cols },
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

bool LezClient::loadState()
{
    QFile f(stateFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return false;   // true first run — caller may seed defaults (#15)
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        // corrupt state is preserved for inspection, never silently discarded
        f.close();
        QFile::remove(stateFilePath() + QStringLiteral(".corrupt"));
        QFile::rename(stateFilePath(), stateFilePath() + QStringLiteral(".corrupt"));
        emit errorOccurred(QStringLiteral("state_corrupt"));
        return true;   // a corrupt file is still "had state" — never re-seed over it
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
        ch.label = jsonStr(co, "label");
        ch.startSlot = co.value(QLatin1String("startSlot")).toVariant().toLongLong();
        ch.cursor = co.value(QLatin1String("cursor")).toVariant().toLongLong();
        ch.lastInscription = co.value(QLatin1String("lastInscription")).toVariant().toLongLong();
        ch.curator = jsonStr(co, "curator");
        ch.synced = co.value(QLatin1String("synced")).toBool();
        ch.autoPreserve = co.value(QLatin1String("autoPreserve")).toBool();
        // migration: state persisted before the collections→items rename (#13)
        const QJsonArray itemRows = co.contains(QLatin1String("items"))
                                        ? co.value(QLatin1String("items")).toArray()
                                        : co.value(QLatin1String("collections")).toArray();
        for (const QJsonValue& colv : itemRows) {
            const QJsonObject c = colv.toObject();
            Item col;
            col.id = jsonStr(c, "id");
            col.title = jsonStr(c, "title");
            col.channelId = ch.channelId;
            col.cid = jsonStr(c, "cid");
            col.sizeBytes = c.value(QLatin1String("sizeBytes")).toVariant().toLongLong();
            col.items = c.value(QLatin1String("items")).toVariant().toLongLong();
            col.thumbnail = jsonStr(c, "thumbnail");
            col.inscribedAtSlot = c.value(QLatin1String("inscribedAt")).toVariant().toLongLong();
            col.txHash = jsonStr(c, "txHash");
            col.blockHash = jsonStr(c, "blockHash");
            // rows persisted before #9 lack txIndex — -1 keeps the explorer
            // resolve on its block-page fallback instead of a wrong-index join
            col.txIndex = c.contains(QLatin1String("txIndex"))
                              ? c.value(QLatin1String("txIndex")).toVariant().toLongLong()
                              : -1;
            col.curator = jsonStr(c, "curator");
            col.state = jsonStr(c, "state");
            col.iaId = jsonStr(c, "iaId");
            col.iaFile = jsonStr(c, "iaFile");
            for (const QJsonValue& sv : c.value(QLatin1String("storedCids")).toArray())
                col.storedCids.append(sv.toString());
            // migration: rows persisted before the iaId feature — the title IS the
            // inscribed label, so the IA source is still derivable
            if (col.iaId.isEmpty())
                deriveIaRef(col.cid, col.title, &col.iaId, &col.iaFile);
            if (col.state == QLatin1String("mirroring"))   // no pin survives a restart
                col.state = QStringLiteral("available");
            if (col.state == QLatin1String("error"))       // errors aren't a property of
                col.state = QStringLiteral("available");   // the item — retryable
            ch.items.append(col);
        }
        if (!ch.channelId.isEmpty())
            m_channels.insert(ch.channelId, ch);
    }
    if (!m_channels.isEmpty()) {
        emit channelsChanged();
        emit itemsChanged();
    }
    return true;
}
