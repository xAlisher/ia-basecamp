#ifndef ARCHIVE_PLUGIN_H
#define ARCHIVE_PLUGIN_H

#include <QHash>
#include <QString>
#include "archive_interface.h"
#include "LogosViewPluginBase.h"
#include "rep_archive_source.h"

class LogosAPI;
class QTimer;
class LezClient;
class StorageClient;

// ui_qml module with a C++ backend (logos-delivery-demo / radio_app shape). Pure HTTP/JSON-RPC
// client — reads the LEZ indexer and a Storage endpoint behind a trusted gateway. No platform-module
// deps (no delivery#31), builds cross-platform. SLOT bodies land in P1 (read) / P2 (preserve).
class ArchivePlugin : public ArchiveSimpleSource,
                      public ArchiveInterface,
                      public ArchiveViewPluginBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ArchiveInterface_iid FILE "metadata.json")
    Q_INTERFACES(ArchiveInterface)

public:
    explicit ArchivePlugin(QObject* parent = nullptr);
    ~ArchivePlugin() override;

    QString name()    const override { return "archive"; }
    QString version() const override { return "0.1.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    // .rep SLOTs
    QString setGateways(QString jsonList) override;
    QString choosePreserveMode(QString mode) override;
    QString getStatus() override;
    QString followChannel(QString channelRef) override;
    QString unfollowChannel(QString channelId) override;
    QString refreshChannel(QString channelId) override;
    QString getCollections(QString channelId) override;
    QString mirrorCollection(QString collectionId) override;
    QString unmirrorCollection(QString collectionId) override;
    QString getMirrorStatus(QString collectionId) override;
    QString getShareData(QString scope) override;
    QString saveShareCard(QString pngBase64, QString name) override;
    QString revealCard(QString path) override;

private:
    void pollGatewayHealth();    // ping gateway node + storage, compute syncLagBlocks (LEZ#519)
    void publishReadState();     // rebuild channelsJson/collectionsJson/summaryJson PROPs
    void syncStorageEndpoint();  // point storage at gateway storageUrl (delegate) or local node

    LogosAPI*      m_logosAPI = nullptr;
    LezClient*     m_lez = nullptr;
    StorageClient* m_storage = nullptr;
    QTimer*        m_healthTimer = nullptr;
    QHash<QString, QString> m_cidToCollection;   // in-flight pin/unpin → collection id
    qint64         m_usedBytes = 0;
};

#endif // ARCHIVE_PLUGIN_H
