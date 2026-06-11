#ifndef LEZ_CLIENT_H
#define LEZ_CLIENT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

// Read side of the module (SPEC §4.1): talks to a gateway node's Cryptarchia HTTP API.
//   GET /cryptarchia/info                          → health + finality (lib_slot) + sync lag
//   GET /cryptarchia/blocks?slot_from&slot_to      → scan ChannelInscribe (opcode 17) ops
// Finalized-only: scans never go past lib_slot. Also owns the persisted follow/config state
// (one state file for the whole module). All HTTP is async; results land via signals.
class LezClient : public QObject {
    Q_OBJECT
public:
    struct Gateway {
        QString nodeUrl;
        QString storageUrl;
    };

    struct Item {
        QString id;          // manifest id, falls back to the L1 tx hash
        QString title;
        QString channelId;
        QString cid;
        qint64  sizeBytes = 0;
        qint64  items = 0;
        QString thumbnail;
        qint64  inscribedAtSlot = 0;
        QString txHash;      // node mantle_tx.hash — NOT the explorer's hash (#9)
        QString blockHash;   // L1 block header id — join key to the explorer
        qint64  txIndex = -1;   // position within the block — explorer index_in_block
        QString curator;     // inscription signer
        QString state;       // available | mirroring | mirrored | error (P2 drives this)
        qint64  progressBlocks = 0;   // transient pin progress while state == mirroring
        QString iaId;        // Internet Archive identifier (keeper cid_pin convention)
        QString iaFile;      // file within the IA item — empty = whole item
    };

    // Why-was-it-skipped counters for the last scan — every defensive skip in the
    // read path is counted, never silent (#11). Runtime-only, not persisted.
    struct ScanStats {
        qint64 scannedSlots = 0;
        int    pages = 0;
        int    matched = 0;
        int    skippedNotFinalized = 0;   // block past lib_slot
        int    skippedNonJson = 0;        // our channel, payload not a JSON object
        int    skippedOversized = 0;      // inscription > kMaxInscriptionBytes
        int    skippedNoCid = 0;          // manifest without a cid
        int    skippedDuplicate = 0;      // (txHash,id) already collected
        int    otherChannelOps = 0;       // inscriptions for channels we don't follow
        int    oversizedBodies = 0;       // /blocks response > kMaxBlocksBodyBytes
    };

    struct Channel {
        QString channelId;
        QString label;             // user-given name (edit icon in the UI)
        qint64  startSlot = 0;     // where history scanning begins
        qint64  cursor = 0;        // last finalized slot already scanned
        qint64  lastInscription = 0;
        QString curator;
        bool    synced = false;    // cursor caught up to lib_slot at last refresh
        qint64  lastLibSlot = 0;   // lib at the most recent scan — drives the progress %
        bool    autoPreserve = false;   // #17: preserve new items as they appear
        ScanStats lastScan;
        QVector<Item> items;
    };

    static constexpr qint64 kPageSlots = 2000;     // slots per /blocks request
    static constexpr int    kMaxPagesPerRefresh = 100;
    // Explorer is an optimizer (scan bounds) and link target only — never a data
    // source for inscriptions; the gateway node stays the source of truth (#10)
    static constexpr const char* kExplorerBase = "https://logosblocks.noders.services";
    static constexpr qint64 kLagDegradedThreshold = 1200;  // ~2× the normal in-flight window
    static constexpr int    kHttpTimeoutMs = 10000;
    // Defensive caps on untrusted gateway responses
    static constexpr qint64 kMaxBlocksBodyBytes = 32 * 1024 * 1024;
    static constexpr int    kMaxInscriptionBytes = 256 * 1024;

    explicit LezClient(QObject* parent = nullptr);

    // config
    void setGateways(const QList<Gateway>& gws);
    QList<Gateway> gateways() const { return m_gateways; }
    // empty disables explorer-assisted scan bounds (tests point this at MockNode)
    void setExplorerBase(const QString& base) { m_explorerBase = base; }
    int activeGateway() const { return m_active; }
    QString preserveMode() const { return m_preserveMode; }
    void setPreserveMode(const QString& mode);

    // health: async; result via healthChanged()
    void pollHealth();
    QString gatewayState() const { return m_gatewayState; }
    qint64 syncLagSlots() const { return m_syncLag; }

    // channels. followChannel parses `ref` (64-hex id, optional "@<startSlot>", or a URL
    // containing the id) and starts the initial scan. Errors via *errorCode.
    QString followChannel(const QString& ref, QString* errorCode = nullptr);
    bool unfollowChannel(const QString& channelId);
    bool refreshChannel(const QString& channelId);   // async scan; false if not followed/busy
    bool isFollowed(const QString& channelId) const { return m_channels.contains(channelId); }
    bool setChannelLabel(const QString& channelId, const QString& label);
    bool setAutoPreserve(const QString& channelId, bool on);   // #17
    bool autoPreserve(const QString& channelId) const
    { return m_channels.value(channelId.toLower()).autoPreserve; }
    QStringList followedChannelIds() const { return m_channels.keys(); }

    QJsonArray channelsJson() const;
    QJsonArray itemsJson(const QString& channelId = QString()) const;
    QJsonObject summaryJson() const;
    QJsonObject scanDiagnosticsJson(const QString& channelId) const;   // last scan's skip counters

    // mirror bookkeeping (P2 — storage results land back on the item records)
    QString itemCid(const QString& itemId) const;   // empty if unknown
    QString itemState(const QString& itemId) const;
    bool setItemState(const QString& itemId, const QString& state,
                            qint64 progressBlocks = -1);

    // persistence (one file: gateways, preserveMode, channels+items).
    // loadState returns false when no state file existed — a true first run
    // (the plugin seeds the official campaign channel exactly then, #15)
    bool loadState();
    void saveState() const;
    static QString stateFilePath();

    // exposed for tests
    static void deriveIaRef(const QString& cid, const QString& label,
                            QString* iaId, QString* iaFile);
    // all plausible (id, file) splits, most-likely first, as "id|file" strings —
    // keeper's "keeper-{id}-{file}" is ambiguous when the filename contains '-'
    static QStringList deriveIaCandidates(const QString& cid, const QString& label);
    static QString parseChannelRef(const QString& ref, qint64* startSlot, QString* errorCode);
    static QVector<Item> extractItems(const QJsonArray& blocks,
                                                  const QString& channelId, qint64 libSlot,
                                                  ScanStats* stats = nullptr);

signals:
    void healthChanged(const QString& state, qint64 lagSlots);
    void channelsChanged();
    void itemsChanged();
    // genuinely-new items from a scan page — drives auto-preserve (#17)
    void itemsDiscovered(const QString& channelId, const QStringList& itemIds);
    void scanFinished(const QString& channelId, bool reachedLib);
    void errorOccurred(const QString& code);

private:
    // One scan = one context QObject (#11): every connect() targets it and every
    // reply is parented to it, so deleting the context cancels everything in
    // flight by object lifetime — no generation counters to re-check per hop.
    void startScan(const QString& channelId);
    // #10: resolve the channel's first-seen slot from the explorer before the first
    // scan — falls through to fetchInfoAndScan unchanged on any failure
    void resolveScanStart(const QString& channelId, QObject* ctx, int gatewayIdx);
    void fetchInfoAndScan(const QString& channelId, QObject* ctx, int gatewayIdx);
    void scanNextPage(const QString& channelId, QObject* ctx, int gatewayIdx,
                      qint64 libSlot, int pagesLeft);
    void endScan(const QString& channelId, QObject* ctx);
    // gatewayIdx -1 = current active; scans pin their gateway so one consistent
    // finalized view drives the whole pagination
    QNetworkReply* httpGet(const QString& path, const QString& query = QString(),
                           int gatewayIdx = -1);
    QNetworkReply* httpGetUrl(const QUrl& url);   // absolute-URL GET (explorer calls)
    void failOver(const QString& code);

    QNetworkAccessManager* m_net = nullptr;
    QList<Gateway> m_gateways;
    QString m_explorerBase = QString::fromLatin1(kExplorerBase);
    int m_active = 0;
    // "local" is the only live mode — preserve goes to Logos Storage via the platform
    // storage_module; "delegate" returns when campaign gateways exist
    QString m_preserveMode = QStringLiteral("local");

    QString m_gatewayState = QStringLiteral("offline");
    qint64 m_syncLag = 0;

    QMap<QString, Channel> m_channels;
    QMap<QString, QObject*> m_scanning;   // channelId → context owning the in-flight scan
};

#endif // LEZ_CLIENT_H
