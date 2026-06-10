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

    struct Collection {
        QString id;          // manifest id, falls back to the L1 tx hash
        QString title;
        QString channelId;
        QString cid;
        qint64  sizeBytes = 0;
        qint64  items = 0;
        QString thumbnail;
        qint64  inscribedAtSlot = 0;
        QString txHash;
        QString curator;     // inscription signer
        QString state;       // available | mirroring | mirrored | error (P2 drives this)
    };

    struct Channel {
        QString channelId;
        qint64  startSlot = 0;     // where history scanning begins
        qint64  cursor = 0;        // last finalized slot already scanned
        qint64  lastInscription = 0;
        QString curator;
        bool    synced = false;    // cursor caught up to lib_slot at last refresh
        QVector<Collection> collections;
    };

    static constexpr qint64 kPageSlots = 2000;     // slots per /blocks request
    static constexpr int    kMaxPagesPerRefresh = 100;
    static constexpr qint64 kLagDegradedThreshold = 1200;  // ~2× the normal in-flight window
    static constexpr int    kHttpTimeoutMs = 10000;

    explicit LezClient(QObject* parent = nullptr);

    // config
    void setGateways(const QList<Gateway>& gws);
    QList<Gateway> gateways() const { return m_gateways; }
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

    QJsonArray channelsJson() const;
    QJsonArray collectionsJson(const QString& channelId = QString()) const;
    QJsonObject summaryJson() const;

    // persistence (one file: gateways, preserveMode, channels+collections)
    void loadState();
    void saveState() const;
    static QString stateFilePath();

    // exposed for tests
    static QString parseChannelRef(const QString& ref, qint64* startSlot, QString* errorCode);
    static QVector<Collection> extractCollections(const QJsonArray& blocks,
                                                  const QString& channelId, qint64 libSlot);

signals:
    void healthChanged(const QString& state, qint64 lagSlots);
    void channelsChanged();
    void collectionsChanged();
    void scanFinished(const QString& channelId, bool reachedLib);
    void errorOccurred(const QString& code);

private:
    void startScan(const QString& channelId);
    void scanNextPage(const QString& channelId, qint64 libSlot, int pagesLeft);
    QNetworkReply* httpGet(const QString& path);
    void failOver(const QString& code);

    QNetworkAccessManager* m_net = nullptr;
    QList<Gateway> m_gateways;
    int m_active = 0;
    QString m_preserveMode = QStringLiteral("delegate");

    QString m_gatewayState = QStringLiteral("offline");
    qint64 m_syncLag = 0;

    QMap<QString, Channel> m_channels;
    QSet<QString> m_scanning;        // reentrancy guard, one scan per channel
};

#endif // LEZ_CLIENT_H
