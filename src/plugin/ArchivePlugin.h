#pragma once

#include <memory>

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

#include "interface.h"
#include "ia_files.h"

class LogosAPI;
class QNetworkAccessManager;
class QTimer;
class LezClient;
class StorageClient;
class StorageTransport;

/**
 * archive — follow curated LEZ channels + preserve their items to Logos Storage.
 *
 * Core module (logos_host), stash's shape: this process owns the gateway HTTP reads
 * (lez_client), the storage_module typed-SDK talk (logos_storage_transport — getClient
 * from logos_host is the proven path; from ui-host it ABI-crashes, see
 * docs/spikes/uihost-getclient-abi-crash.md), and the persisted follow state.
 * The UI is a view-only QML plugin (plugins/archive_ui) polling via logos.callModule.
 *
 * Every Q_INVOKABLE returns a JSON string: {"ok":true,...} or {"ok":false,"error":code}.
 */
class ArchivePlugin : public QObject, public PluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.logos.ArchiveInterface" FILE "plugin_metadata.json")
    Q_INTERFACES(PluginInterface)

public:
    explicit ArchivePlugin(QObject* parent = nullptr);
    ~ArchivePlugin() override;

    QString name()    const override { return QStringLiteral("archive"); }
    QString version() const override { return QStringLiteral("0.2.0"); }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    // ── status (the UI's 2s poll) ────────────────────────────────────────────
    // {ok, gatewayState, syncLagBlocks, storageState, preserveMode, lastError, summary}
    Q_INVOKABLE QString getStatus();

    // ── config ───────────────────────────────────────────────────────────────
    Q_INVOKABLE QString setGateways(const QString& jsonList);   // [{nodeUrl}] failover order
    Q_INVOKABLE QString choosePreserveMode(const QString& mode);

    // ── channels (read the LEZ) ──────────────────────────────────────────────
    Q_INVOKABLE QString followChannel(const QString& channelRef);
    Q_INVOKABLE QString unfollowChannel(const QString& channelId);
    Q_INVOKABLE QString refreshChannel(const QString& channelId);
    Q_INVOKABLE QString getChannels();                          // {ok, channels:[...]}
    Q_INVOKABLE QString getScanDiagnostics(const QString& channelId);   // last scan's skip counters (#11)
    Q_INVOKABLE QString setChannelLabel(const QString& channelId, const QString& label);

    // ── items + preserve (Logos Storage) ───────────────────────────────
    Q_INVOKABLE QString getItems(const QString& channelId);  // "" = all followed
    Q_INVOKABLE QString mirrorItem(const QString& itemId);
    Q_INVOKABLE QString unmirrorItem(const QString& itemId);
    Q_INVOKABLE QString getMirrorStatus(const QString& itemId);

    // ── share cards (SPEC §12) ───────────────────────────────────────────────
    Q_INVOKABLE QString getShareData(const QString& scope);
    Q_INVOKABLE QString saveShareCard(const QString& pngBase64, const QString& name);
    Q_INVOKABLE QString finalizeShareCard(const QString& tmpPath, const QString& name);
    Q_INVOKABLE QString revealCard(const QString& path);
    // browser via system opener; blockHash+txIndex resolve the explorer's own tx
    // hash (node hash is not indexed there, #9) — block page is the fallback
    Q_INVOKABLE QString openExplorerTx(const QString& txHash,
                                       const QString& blockHash = QString(),
                                       int txIndex = -1);

signals:
    // ModuleProxy requires this exact signal on the concrete class —
    // without it every callModule returns {"error":"Invalid response"}
    void eventResponse(const QString& eventName, const QVariantList& data);

private:
    static QString cardsDir();
    void pollGatewayHealth();

    LogosAPI*      m_logosAPI = nullptr;
    LezClient*     m_lez = nullptr;
    std::unique_ptr<StorageTransport> m_transport;   // Logos Storage via storage_module
    StorageClient* m_storage = nullptr;
    QTimer*        m_healthTimer = nullptr;
    QHash<QString, QString> m_cidToItem;   // in-flight pin/unpin → item id
    qint64         m_usedBytes = 0;
    QString        m_lastError;

    // preserve-by-reseed (keeper's flow): fetch failed → download the file from the
    // Internet Archive → upload into Logos Storage (we become the provider)
    void startReseed(const QString& itemId, const QString& cid);
    void tryReseedCandidate(const QString& itemId);   // walks m_reseedCandidates
    QNetworkAccessManager* m_nam = nullptr;
    QString m_reseedItemId;   // one re-seed at a time (upload contract)
    QString m_reseedCid;
    QString m_reseedTmpPath;
    QStringList m_reseedCandidates; // "id|file" splits still untried

    // campaign ia_item preserve (#14): {id}_files.xml → per file: download with
    // keeper-exact naming → verify md5/sha1 → upload. Sequential — one item, one
    // file in flight (the storage upload contract). State below IS the machine.
    void startIaPreserve(const QString& itemId, const QString& iaId);
    void iaNextFile();
    void iaFail(const QString& error);
    QString m_iaItemId;             // "" = idle
    QString m_iaIaId;
    QList<IaFileEntry> m_iaFiles;
    int m_iaIdx = 0;
    int m_iaUnverified = 0;
    QString m_iaTmpPath;
};
