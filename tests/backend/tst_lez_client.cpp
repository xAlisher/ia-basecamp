#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "lez_client.h"
#include "mock_node.h"

// Backend tests per SPEC §13: deterministic, no network, mock gateway only.
// State isolation: each test points XDG_DATA_HOME at a fresh temp dir.

namespace {

const QString kChannel =
    QStringLiteral("8edab686b441eac68b194445a5052b65812ed25d68abe582824cadab99d5bf31");
const QString kOtherChannel =
    QStringLiteral("1111111111111111111111111111111111111111111111111111111111111111");

QJsonArray toByteArray(const QByteArray& bytes)
{
    QJsonArray arr;
    for (const char b : bytes)
        arr.append(static_cast<int>(static_cast<unsigned char>(b)));
    return arr;
}

QJsonObject inscribeOp(const QString& channelId, const QByteArray& inscription,
                       const QString& signer)
{
    return QJsonObject{
        { "opcode", 17 },
        { "payload", QJsonObject{ { "channel_id", channelId },
                                  { "inscription", toByteArray(inscription) },
                                  { "parent", QStringLiteral("aa").repeated(32) },
                                  { "signer", signer } } },
    };
}

QJsonObject block(qint64 slot, const QString& txHash, const QJsonArray& ops)
{
    return QJsonObject{
        { "header", QJsonObject{ { "slot", slot }, { "id", QStringLiteral("b%1").arg(slot) } } },
        { "transactions", QJsonArray{ QJsonObject{
              { "mantle_tx", QJsonObject{ { "hash", txHash }, { "ops", ops } } } } } },
    };
}

QByteArray manifest(const char* id, const char* title, const char* cid, qint64 size)
{
    const QJsonObject m{ { "v", 1 },         { "type", "item" }, { "id", id },
                         { "title", title }, { "cid", cid },           { "sizeBytes", size },
                         { "items", 3 },     { "thumbnail", "thumb-cid" } };
    return QJsonDocument(m).toJson(QJsonDocument::Compact);
}

} // namespace

class TstLezClient : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* m_dataDir = nullptr;
    MockNode* m_node = nullptr;

    LezClient* makeClient()
    {
        auto* c = new LezClient(this);
        c->setGateways({ { m_node->baseUrl(), QString() } });
        // explorer-assisted scan bounds off by default — tests opting in point
        // the base at MockNode's /api/* stand-ins (never the real explorer)
        c->setExplorerBase(QString());
        return c;
    }

    // Attach BEFORE triggering follow/refresh — a spy created after the async kick-off
    // can miss a fast scanFinished and time out spuriously.
    struct ScanWaiter {
        QSignalSpy spy;
        explicit ScanWaiter(LezClient* c) : spy(c, &LezClient::scanFinished) {}
        bool wait(const QString& channelId, int timeoutMs = 5000)
        {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < timeoutMs) {
                for (const QList<QVariant>& args : spy)
                    if (args.at(0).toString() == channelId)
                        return true;
                spy.clear();
                spy.wait(200);
            }
            return false;
        }
    };

private slots:
    void init()
    {
        m_dataDir = new QTemporaryDir;
        QVERIFY(m_dataDir->isValid());
        qputenv("XDG_DATA_HOME", m_dataDir->path().toUtf8());

        m_node = new MockNode(this);
        QVERIFY(m_node->start());
        m_node->info = QJsonObject{ { "slot", 10500 }, { "lib_slot", 10000 },
                                    { "tip", "t" },    { "mode", "Online" } };
    }

    void cleanup()
    {
        delete m_node;
        m_node = nullptr;
        delete m_dataDir;
        m_dataDir = nullptr;
    }

    // ── ref parsing ─────────────────────────────────────────────────────────

    void parseRef_plainHex()
    {
        qint64 slot = -1;
        QString err;
        QCOMPARE(LezClient::parseChannelRef(kChannel.toUpper(), &slot, &err), kChannel);
        QCOMPARE(slot, 0);
    }

    void parseRef_withStartSlot()
    {
        qint64 slot = 0;
        QString err;
        QCOMPARE(LezClient::parseChannelRef(kChannel + "@4700000", &slot, &err), kChannel);
        QCOMPARE(slot, qint64(4700000));
    }

    void parseRef_url()
    {
        qint64 slot = 0;
        QString err;
        const QString url = "https://example.org/web/explorer/channels/" + kChannel + "?x=1";
        QCOMPARE(LezClient::parseChannelRef(url, &slot, &err), kChannel);
    }

    void parseRef_invalid()
    {
        qint64 slot = 0;
        QString err;
        QVERIFY(LezClient::parseChannelRef("not-a-channel", &slot, &err).isEmpty());
        QCOMPARE(err, QStringLiteral("invalid_channel_ref"));
    }

    // ── decode ──────────────────────────────────────────────────────────────

    void extract_decodesMatchingInscriptions()
    {
        // one matching, one other-channel, one non-JSON payload on the right channel
        QJsonArray ops;
        ops.append(inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 42), "sig-1"));
        ops.append(inscribeOp(kOtherChannel, manifest("cx", "Other", "cid-x", 1), "sig-x"));
        ops.append(inscribeOp(kChannel, QByteArray("\x01\x02\x03binary"), "sig-1"));
        const QJsonArray blocks{ block(9000, "tx-1", ops) };

        const auto cols = LezClient::extractItems(blocks, kChannel, 10000);
        QCOMPARE(cols.size(), 1);
        QCOMPARE(cols[0].id, QStringLiteral("c1"));
        QCOMPARE(cols[0].title, QStringLiteral("Maps"));
        QCOMPARE(cols[0].cid, QStringLiteral("cid-1"));
        QCOMPARE(cols[0].sizeBytes, qint64(42));
        QCOMPARE(cols[0].items, qint64(3));
        QCOMPARE(cols[0].thumbnail, QStringLiteral("thumb-cid"));
        QCOMPARE(cols[0].txHash, QStringLiteral("tx-1"));
        QCOMPARE(cols[0].blockHash, QStringLiteral("b9000"));   // explorer join key (#9)
        QCOMPARE(cols[0].txIndex, qint64(0));
        QCOMPARE(cols[0].curator, QStringLiteral("sig-1"));
        QCOMPARE(cols[0].inscribedAtSlot, qint64(9000));
        QCOMPARE(cols[0].state, QStringLiteral("available"));
    }

    void extract_txIndexTracksPositionInBlock()
    {
        // the explorer indexes its own tx hash by position — a wrong index would
        // silently open someone else's transaction (#9)
        const QJsonObject multiTx{
            { "header", QJsonObject{ { "slot", 9100 }, { "id", "b9100" } } },
            { "transactions", QJsonArray{
                  QJsonObject{ { "mantle_tx", QJsonObject{
                        { "hash", "tx-other" },
                        { "ops", QJsonArray{ inscribeOp(kOtherChannel,
                              manifest("cx", "Other", "cid-x", 1), "sx") } } } } },
                  QJsonObject{ { "mantle_tx", QJsonObject{
                        { "hash", "tx-ours" },
                        { "ops", QJsonArray{ inscribeOp(kChannel,
                              manifest("c2", "Mine", "cid-2", 2), "s2") } } } } },
              } },
        };
        const auto cols = LezClient::extractItems({ multiTx }, kChannel, 10000);
        QCOMPARE(cols.size(), 1);
        QCOMPARE(cols[0].blockHash, QStringLiteral("b9100"));
        QCOMPARE(cols[0].txIndex, qint64(1));
    }

    void scan_explorerBoundsFreshFollow()
    {
        // inscription 599k slots deep; one refresh covers kPageSlots×kMaxPages=200k,
        // so a genesis crawl cannot reach it — only the explorer bound can (#10)
        const QString hexId = QStringLiteral("ab").repeated(32);
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "Deep", "cid-1", 1), "sig") };
        m_node->blocks = QJsonArray{ QJsonObject{
            { "header", QJsonObject{ { "slot", 599000 }, { "id", hexId } } },
            { "transactions", QJsonArray{ QJsonObject{ { "mantle_tx",
                  QJsonObject{ { "hash", "tx-deep" }, { "ops", ops } } } } } },
        } };
        m_node->info = QJsonObject{ { "slot", 600400 }, { "lib_slot", 600000 },
                                    { "tip", "t" },     { "mode", "Online" } };
        m_node->channelInfo = QJsonObject{ { "first_seen_block_id", hexId },
                                           { "last_seen_slot", 599000 } };

        LezClient* c = makeClient();
        c->setExplorerBase(m_node->baseUrl());
        ScanWaiter waiter(c);
        c->followChannel(kChannel);   // deliberately no @slot hint
        QVERIFY(waiter.wait(kChannel));
        const QJsonArray cols = c->itemsJson();
        QCOMPARE(cols.size(), 1);
        QCOMPARE(cols.at(0).toObject().value("blockHash").toString(), hexId);
        QCOMPARE(cols.at(0).toObject().value("txIndex").toInt(), 0);
    }

    void scan_explorerUnreachableFallsBack()
    {
        // explorer 404s (empty channelInfo) — fresh follow scans from genesis as
        // before; inscription within the first refresh window is still found
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "Near", "cid-1", 1), "sig") };
        m_node->blocks = QJsonArray{ block(9000, "tx-1", ops) };

        LezClient* c = makeClient();
        c->setExplorerBase(m_node->baseUrl());   // mock returns 404 for /api/channel/*
        ScanWaiter waiter(c);
        c->followChannel(kChannel);
        QVERIFY(waiter.wait(kChannel));
        QCOMPARE(c->itemsJson().size(), 1);
    }

    void scan_emitsItemsDiscovered()
    {
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 1), "s") };
        m_node->blocks = QJsonArray{ block(9000, "tx-1", ops) };

        LezClient* c = makeClient();
        QSignalSpy disc(c, &LezClient::itemsDiscovered);
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@7000");
        QVERIFY(waiter.wait(kChannel));
        QCOMPARE(disc.count(), 1);
        QCOMPARE(disc.first().at(0).toString(), kChannel);
        QCOMPARE(disc.first().at(1).toStringList(), QStringList{ "c1" });
    }

    void autoPreserve_persistsAcrossReload()
    {
        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@7000");
        QVERIFY(waiter.wait(kChannel));
        QVERIFY(!c->autoPreserve(kChannel));
        QVERIFY(c->setAutoPreserve(kChannel, true));
        QVERIFY(c->autoPreserve(kChannel));
        QVERIFY(!c->setAutoPreserve("ffff", true));   // unknown channel refused

        // bare construction — makeClient's setGateways saves (empty) state and
        // would clobber the file before loadState gets to read it
        auto* c2 = new LezClient(this);
        QVERIFY(c2->loadState());
        QVERIFY(c2->autoPreserve(kChannel));
    }

    void scan_diagnosticsCountEverySkip()
    {
        // #11: nothing the scan drops is silent — each skip lands in a counter
        QJsonArray ops;
        ops.append(inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 42), "sig"));
        ops.append(inscribeOp(kOtherChannel, manifest("cx", "Other", "cid-x", 1), "sx"));
        ops.append(inscribeOp(kChannel, QByteArray("\x01\x02\x03binary"), "sig"));
        m_node->blocks = QJsonArray{ block(9000, "tx-1", ops) };

        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@7000");
        QVERIFY(waiter.wait(kChannel));

        const QJsonObject d = c->scanDiagnosticsJson(kChannel);
        QCOMPARE(d.value("known").toBool(), true);
        QCOMPARE(d.value("matched").toInt(), 1);
        QCOMPARE(d.value("skippedNonJson").toInt(), 1);
        QCOMPARE(d.value("otherChannelOps").toInt(), 1);
        QCOMPARE(d.value("skippedDuplicate").toInt(), 0);
        QCOMPARE(d.value("scannedSlots").toVariant().toLongLong(), qint64(3001));
        QCOMPARE(d.value("pages").toInt(), 2);

        QCOMPARE(c->scanDiagnosticsJson("ffff").value("known").toBool(), false);
    }

    void extract_finalizedOnly()
    {
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "T", "cid-1", 1), "s") };
        const QJsonArray blocks{ block(10001, "tx-late", ops) };  // past lib_slot 10000
        QCOMPARE(LezClient::extractItems(blocks, kChannel, 10000).size(), 0);
    }

    void extract_iaItemPayload()
    {
        // campaign payload v2 (#14): no cid — the IA identifier is the content ref
        const QByteArray entry =
            R"({"v":1,"type":"ia_item","id":"photo-metro-august-1991","name":"Metro photos","size":43386})";
        QJsonArray ops{ inscribeOp(kChannel, entry, "campaign-signer") };
        const auto items = LezClient::extractItems({ block(9000, "tx-ia", ops) }, kChannel, 10000);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].cid, QString());
        QCOMPARE(items[0].iaId, QStringLiteral("photo-metro-august-1991"));
        QCOMPARE(items[0].id, QStringLiteral("photo-metro-august-1991"));
        QCOMPARE(items[0].title, QStringLiteral("Metro photos"));
        QCOMPARE(items[0].sizeBytes, qint64(43386));
        QVERIFY(items[0].iaFile.isEmpty());   // whole item, not a single file
    }

    void extract_iaItemWithoutIdSkipped()
    {
        LezClient::ScanStats st;
        QJsonArray ops{ inscribeOp(kChannel, R"({"v":1,"type":"ia_item","name":"x"})", "s") };
        QCOMPARE(LezClient::extractItems({ block(9000, "t", ops) }, kChannel, 10000, &st).size(), 0);
        QCOMPARE(st.skippedNoCid, 1);   // no id = not preservable, and counted
    }

    void extract_permissiveManifest_cidOnly()
    {
        // keeper-style {"type":"cid_pin","cid":...} — degenerate but preservable
        const QByteArray pin = R"({"v":1,"type":"cid_pin","cid":"ia:kuMUquaeE6g","source":"keeper"})";
        QJsonArray ops{ inscribeOp(kChannel, pin, "s") };
        const auto cols = LezClient::extractItems({ block(9000, "tx-p", ops) }, kChannel, 10000);
        QCOMPARE(cols.size(), 1);
        QCOMPARE(cols[0].cid, QStringLiteral("ia:kuMUquaeE6g"));
        QCOMPARE(cols[0].title, QStringLiteral("ia:kuMUquaeE6g"));  // falls back to cid
        QCOMPARE(cols[0].id, QStringLiteral("tx-p"));               // falls back to txHash
    }

    // ── health ──────────────────────────────────────────────────────────────

    void health_ready()
    {
        LezClient* c = makeClient();
        QSignalSpy spy(c, &LezClient::healthChanged);
        c->pollHealth();
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("ready"));
        QCOMPARE(spy.last().at(1).toLongLong(), qint64(500));  // 10500 - 10000
    }

    void health_degradedOnLag()
    {
        m_node->info.insert("slot", 10000 + 5000);  // lag 5000 > threshold
        LezClient* c = makeClient();
        QSignalSpy spy(c, &LezClient::healthChanged);
        c->pollHealth();
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("degraded"));
    }

    void health_offlineOnRefusal()
    {
        m_node->refuse = true;
        LezClient* c = makeClient();
        QSignalSpy spy(c, &LezClient::healthChanged);
        QSignalSpy errSpy(c, &LezClient::errorOccurred);
        c->pollHealth();
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("offline"));
        QCOMPARE(errSpy.count(), 1);
    }

    // ── follow / scan / refresh ─────────────────────────────────────────────

    void follow_scansHistory()
    {
        QJsonArray ops1{ inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 42), "sig") };
        QJsonArray ops2{ inscribeOp(kChannel, manifest("c2", "Books", "cid-2", 7), "sig") };
        m_node->blocks = QJsonArray{ block(8000, "tx-1", ops1), block(9500, "tx-2", ops2) };

        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        QString err;
        const QString id = c->followChannel(kChannel + "@7000", &err);
        QCOMPARE(id, kChannel);
        QVERIFY(waiter.wait(kChannel));

        const QJsonArray cols = c->itemsJson();
        QCOMPARE(cols.size(), 2);
        const QJsonArray chans = c->channelsJson();
        QCOMPARE(chans.size(), 1);
        QCOMPARE(chans[0].toObject().value("synced").toBool(), true);
        QCOMPARE(chans[0].toObject().value("items").toInt(), 2);
        QCOMPARE(chans[0].toObject().value("lastInscription").toInt(), 9500);
        QCOMPARE(c->summaryJson().value("following").toInt(), 1);
        QCOMPARE(c->summaryJson().value("items").toInt(), 2);
    }

    void follow_rejectsDuplicate()
    {
        LezClient* c = makeClient();
        QString err;
        QVERIFY(!c->followChannel(kChannel, &err).isEmpty());
        QVERIFY(c->followChannel(kChannel, &err).isEmpty());
        QCOMPARE(err, QStringLiteral("already_followed"));
    }

    void refresh_continuesFromCursor()
    {
        QJsonArray ops1{ inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 42), "sig") };
        m_node->blocks = QJsonArray{ block(8000, "tx-1", ops1) };

        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@7000");
        QVERIFY(waiter.wait(kChannel));
        QCOMPARE(c->itemsJson().size(), 1);

        // chain advances; a new inscription lands past the old lib
        QJsonArray ops2{ inscribeOp(kChannel, manifest("c2", "Books", "cid-2", 7), "sig") };
        m_node->blocks.append(block(10800, "tx-2", ops2));
        m_node->info = QJsonObject{ { "slot", 11500 }, { "lib_slot", 11000 },
                                    { "tip", "t" },    { "mode", "Online" } };

        const int before = m_node->requestCount;
        ScanWaiter waiter2(c);
        QVERIFY(c->refreshChannel(kChannel));
        QVERIFY(waiter2.wait(kChannel));
        QCOMPARE(c->itemsJson().size(), 2);
        // resumed from the cursor: only info + the delta pages, not a rescan of 7000+
        QVERIFY(m_node->requestCount - before <= 3);
    }

    void unfollow_dropsItems()
    {
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 1), "sig") };
        m_node->blocks = QJsonArray{ block(9000, "tx-1", ops) };
        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@8000");
        QVERIFY(waiter.wait(kChannel));
        QVERIFY(c->unfollowChannel(kChannel));
        QCOMPARE(c->itemsJson().size(), 0);
        QCOMPARE(c->channelsJson().size(), 0);
        QVERIFY(!c->unfollowChannel(kChannel));
    }

    void refollow_whileScanInFlight()
    {
        // Senty P1 finding: an in-flight scan surviving unfollow must not adopt
        // (or corrupt) a re-created channel of the same id.
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "Old", "cid-1", 1), "sig") };
        m_node->blocks = QJsonArray{ block(1000, "tx-1", ops) };

        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@500");        // multi-page scan begins (lib 10000)
        QVERIFY(c->unfollowChannel(kChannel));      // while page 1 is in flight
        QCOMPARE(c->followChannel(kChannel + "@9000"), kChannel);  // re-follow, new generation

        QVERIFY(waiter.wait(kChannel, 10000));
        // the new scan starts at 9000 — the old generation's tx-1 (slot 1000) must not leak in
        QCOMPARE(c->itemsJson().size(), 0);
        QCOMPARE(c->channelsJson().size(), 1);
        QCOMPARE(c->channelsJson()[0].toObject().value("synced").toBool(), true);
    }

    void scan_multiPage()
    {
        // inscriptions 5 pages apart force pagination (kPageSlots = 2000)
        QJsonArray ops1{ inscribeOp(kChannel, manifest("c1", "A", "cid-1", 1), "sig") };
        QJsonArray ops2{ inscribeOp(kChannel, manifest("c2", "B", "cid-2", 1), "sig") };
        m_node->blocks = QJsonArray{ block(1000, "tx-1", ops1), block(9900, "tx-2", ops2) };

        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->followChannel(kChannel + "@500");
        QVERIFY(waiter.wait(kChannel, 10000));
        QCOMPARE(c->itemsJson().size(), 2);
    }

    void deriveIaRef_bothConventions()
    {
        QString id, file;
        LezClient::deriveIaRef("ia:anarchy_Cypherpunk_Manifesto", "", &id, &file);
        QCOMPARE(id, QStringLiteral("anarchy_Cypherpunk_Manifesto"));
        QVERIFY(file.isEmpty());

        LezClient::deriveIaRef("zDvZRwzmXx",
            QString::fromUtf8("Logos Storage: keeper-popeye_taxi-turvey-metadata.json \u2192 zDvZRwzmXx"),
            &id, &file);
        QCOMPARE(id, QStringLiteral("popeye_taxi-turvey"));
        QCOMPARE(file, QStringLiteral("metadata.json"));

        // arrow tail truncated away (persisted titles are label.left(120))
        LezClient::deriveIaRef("zDvZ",
            QStringLiteral("Logos Storage: keeper-popeye_big_bad_sinbad-__ia_thumb.jpg"),
            &id, &file);
        QCOMPARE(id, QStringLiteral("popeye_big_bad_sinbad"));
        QCOMPARE(file, QStringLiteral("__ia_thumb.jpg"));

        LezClient::deriveIaRef("zDvZ", "some unrelated label", &id, &file);
        QVERIFY(id.isEmpty());
    }

    void deriveIaCandidates_ambiguousHyphens()
    {
        // "keeper-taxi-turvy-taxi-turvy.mpeg": the true split is taxi-turvy /
        // taxi-turvy.mpeg, but hyphens before the dot make several plausible splits
        const QStringList c = LezClient::deriveIaCandidates("zDvZ",
            QString::fromUtf8("Logos Storage: keeper-taxi-turvy-taxi-turvy.mpeg \u2192 zDvZ"));
        QVERIFY(c.size() >= 2);
        QCOMPARE(c[0], QStringLiteral("taxi-turvy-taxi|turvy.mpeg"));   // first guess
        QVERIFY(c.contains(QStringLiteral("taxi-turvy|taxi-turvy.mpeg")));   // the truth
        QVERIFY(c.size() <= 4);   // archive.org hammering cap
    }

    void state_legacyRowsGainIaRefAndRetry()
    {
        // simulate a state file written before iaId/iaFile existed, with an error row
        const QString dir = QFileInfo(LezClient::stateFilePath()).path();
        QDir().mkpath(dir);
        const QJsonObject legacy{
            { "gateways", QJsonArray{ QJsonObject{ { "nodeUrl", m_node->baseUrl() } } } },
            { "preserveMode", "local" },
            { "channels", QJsonArray{ QJsonObject{
                  { "channelId", kChannel }, { "startSlot", 100 }, { "cursor", 200 },
                  { "items", QJsonArray{ QJsonObject{
                        { "id", "c1" },
                        { "title", "Logos Storage: keeper-popeye_big_bad_sinbad-__ia_thumb.jpg" },
                        { "cid", "zDvZLegacy" },
                        { "state", "error" },
                    } } },
              } } },
        };
        QFile f(LezClient::stateFilePath());
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(legacy).toJson(QJsonDocument::Compact));
        f.close();

        LezClient* c = new LezClient(this);
        c->loadState();
        const QJsonObject col = c->itemsJson()[0].toObject();
        QCOMPARE(col.value("iaId").toString(), QStringLiteral("popeye_big_bad_sinbad"));
        QCOMPARE(col.value("iaFile").toString(), QStringLiteral("__ia_thumb.jpg"));
        QCOMPARE(col.value("state").toString(), QStringLiteral("available"));
    }

    // ── persistence ─────────────────────────────────────────────────────────

    void state_roundTrips()
    {
        QJsonArray ops{ inscribeOp(kChannel, manifest("c1", "Maps", "cid-1", 42), "sig") };
        m_node->blocks = QJsonArray{ block(9000, "tx-1", ops) };

        LezClient* c = makeClient();
        ScanWaiter waiter(c);
        c->setPreserveMode(QStringLiteral("local"));
        c->followChannel(kChannel + "@8000");
        QVERIFY(waiter.wait(kChannel));

        LezClient* c2 = new LezClient(this);
        c2->loadState();
        QCOMPARE(c2->preserveMode(), QStringLiteral("local"));
        QCOMPARE(c2->gateways().size(), 1);
        QCOMPARE(c2->gateways()[0].nodeUrl, m_node->baseUrl());
        QCOMPARE(c2->channelsJson().size(), 1);
        QCOMPARE(c2->itemsJson().size(), 1);
        QCOMPARE(c2->itemsJson()[0].toObject().value("title").toString(),
                 QStringLiteral("Maps"));
    }

    // ── failover ────────────────────────────────────────────────────────────

    void failover_rotatesGateway()
    {
        MockNode dead;
        QVERIFY(dead.start());
        dead.refuse = true;

        LezClient* c = new LezClient(this);
        c->setGateways({ { dead.baseUrl(), QString() }, { m_node->baseUrl(), QString() } });
        QCOMPARE(c->activeGateway(), 0);

        QSignalSpy spy(c, &LezClient::healthChanged);
        c->pollHealth();                       // gateway 0 refuses → offline + rotate
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("offline"));
        QCOMPARE(c->activeGateway(), 1);

        c->pollHealth();                       // gateway 1 answers → ready
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("ready"));
    }

    void health_lagSurfacedNotRotated()
    {
        // #11: a lagging gateway is SURFACED as degraded, no longer rotated away
        // from — the demote-and-rotate state machine was deleted (dead gateways
        // still rotate via failOver, covered by failover_rotatesGateway)
        MockNode laggy;
        QVERIFY(laggy.start());
        laggy.info = QJsonObject{ { "slot", 50000 }, { "lib_slot", 10000 },   // lag 40k
                                  { "tip", "t" },    { "mode", "Online" } };

        LezClient* c = new LezClient(this);
        c->setGateways({ { laggy.baseUrl(), QString() }, { m_node->baseUrl(), QString() } });

        QSignalSpy spy(c, &LezClient::healthChanged);
        c->pollHealth();
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("degraded"));
        QCOMPARE(c->activeGateway(), 0);   // stays put — degraded is a pill, not a demotion
    }

    void scanFailover_cursorSurvivesGatewayDeath()
    {
        // P4: a scan dying mid-history keeps its cursor; the retry on the next
        // gateway resumes from where it stopped instead of rescanning
        QJsonArray ops1{ inscribeOp(kChannel, manifest("c1", "A", "cid-1", 1), "sig") };
        m_node->blocks = QJsonArray{ block(8000, "tx-1", ops1) };

        MockNode dying;
        QVERIFY(dying.start());
        dying.info = m_node->info;
        dying.blocks = m_node->blocks;

        LezClient* c = new LezClient(this);
        c->setGateways({ { dying.baseUrl(), QString() }, { m_node->baseUrl(), QString() } });

        ScanWaiter w1(c);
        c->followChannel(kChannel + "@7000");
        QVERIFY(w1.wait(kChannel));        // first scan completes on gateway 0
        QCOMPARE(c->itemsJson().size(), 1);

        dying.refuse = true;               // gateway 0 dies; chain advances on gateway 1
        QJsonArray ops2{ inscribeOp(kChannel, manifest("c2", "B", "cid-2", 1), "sig") };
        m_node->blocks.append(block(10900, "tx-2", ops2));
        m_node->info = QJsonObject{ { "slot", 11500 }, { "lib_slot", 11000 },
                                    { "tip", "t" },    { "mode", "Online" } };

        ScanWaiter w2(c);
        QVERIFY(c->refreshChannel(kChannel));   // hits dead gateway 0 → rotates
        QVERIFY(w2.wait(kChannel));

        const int healthyBefore = m_node->requestCount;
        ScanWaiter w3(c);
        QVERIFY(c->refreshChannel(kChannel));   // now on gateway 1, resumes from cursor
        QVERIFY(w3.wait(kChannel));
        QCOMPARE(c->itemsJson().size(), 2);
        QVERIFY(m_node->requestCount - healthyBefore <= 3);   // delta only, no rescan
    }

    void scanFailover_diesMidPagination()
    {
        // P4 (Senty): the gateway dies BETWEEN pages of one scan. The scan is pinned to
        // its gateway, fails with the partial cursor persisted, and the next refresh
        // resumes from that cursor on the surviving gateway.
        QJsonArray ops1{ inscribeOp(kChannel, manifest("c1", "A", "cid-1", 1), "sig") };
        QJsonArray ops2{ inscribeOp(kChannel, manifest("c2", "B", "cid-2", 1), "sig") };
        const QJsonArray chain{ block(1000, "tx-1", ops1), block(9900, "tx-2", ops2) };

        MockNode dying;
        QVERIFY(dying.start());
        dying.info = m_node->info;          // lib 10000 → scan from 500 needs 5 pages
        dying.blocks = chain;
        dying.failAfterRequests = 2;        // serves /info + page 1, then dies
        m_node->blocks = chain;

        LezClient* c = new LezClient(this);
        c->setGateways({ { dying.baseUrl(), QString() }, { m_node->baseUrl(), QString() } });

        ScanWaiter w1(c);
        QSignalSpy errSpy(c, &LezClient::errorOccurred);
        c->followChannel(kChannel + "@500");
        QVERIFY(w1.wait(kChannel, 10000));
        QCOMPARE(c->itemsJson().size(), 1);   // page 1 (slot 1000) landed, then death
        QCOMPARE(errSpy.count(), 1);                // surfaced, not silent
        QCOMPARE(c->channelsJson()[0].toObject().value("synced").toBool(), false);

        ScanWaiter w2(c);
        QVERIFY(c->refreshChannel(kChannel));       // rotated to the healthy gateway
        QVERIFY(w2.wait(kChannel, 10000));
        QCOMPARE(c->itemsJson().size(), 2);   // resumed; nothing skipped, no dupes
        QCOMPARE(c->channelsJson()[0].toObject().value("synced").toBool(), true);
    }

    // ── live (opt-in: ARCHIVE_LIVE_NODE=http://host:port) ───────────────────

    void live_readKeeperStyleChannel()
    {
        const QByteArray node = qgetenv("ARCHIVE_LIVE_NODE");
        if (node.isEmpty())
            QSKIP("set ARCHIVE_LIVE_NODE to run the live read");

        // channel + slot from the P0 spike (docs/spikes/p0-channel-read.md)
        const QString chan =
            QStringLiteral("8edab686b441eac68b194445a5052b65812ed25d68abe582824cadab99d5bf31");
        LezClient* c = new LezClient(this);
        c->setGateways({ { QString::fromUtf8(node), QString() } });
        QString err;
        QCOMPARE(c->followChannel(chan + "@4709300", &err), chan);

        // the known inscription sits in the first scan page — don't wait for full sync
        const auto hasSpikeCid = [c] {
            const QJsonArray cols = c->itemsJson();
            return std::any_of(cols.cbegin(), cols.cend(), [](const QJsonValue& v) {
                return v.toObject().value("cid").toString() == QLatin1String("ia:kuMUquaeE6g");
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasSpikeCid(), 60000);
    }
};

QTEST_MAIN(TstLezClient)
#include "tst_lez_client.moc"
