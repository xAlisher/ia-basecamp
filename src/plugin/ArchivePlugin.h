#pragma once

#include <memory>

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

#include "interface.h"

class LogosAPI;
class QTimer;
class LezClient;
class StorageClient;
class StorageTransport;

/**
 * archive — follow curated LEZ channels + preserve their collections to Logos Storage.
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

    // ── collections + preserve (Logos Storage) ───────────────────────────────
    Q_INVOKABLE QString getCollections(const QString& channelId);  // "" = all followed
    Q_INVOKABLE QString mirrorCollection(const QString& collectionId);
    Q_INVOKABLE QString unmirrorCollection(const QString& collectionId);
    Q_INVOKABLE QString getMirrorStatus(const QString& collectionId);

    // ── share cards (SPEC §12) ───────────────────────────────────────────────
    Q_INVOKABLE QString getShareData(const QString& scope);
    Q_INVOKABLE QString saveShareCard(const QString& pngBase64, const QString& name);
    Q_INVOKABLE QString revealCard(const QString& path);

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
    QHash<QString, QString> m_cidToCollection;   // in-flight pin/unpin → collection id
    qint64         m_usedBytes = 0;
    QString        m_lastError;
};
