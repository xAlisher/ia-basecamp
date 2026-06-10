#ifndef STORAGE_CLIENT_H
#define STORAGE_CLIENT_H

#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

// Preserve side of the module (SPEC §5, P2v2): collections are preserved ONLY to Logos
// Storage, through the platform storage_module — the same path stash uses. No HTTP
// storage endpoints, no alternative backends.
//
// StorageTransport isolates the typed-SDK dependency (stash's pattern): the real
// implementation (logos_storage_transport.*) wraps the generated StorageModule client
// and is compiled only into the plugin; tests inject a mock and run headless.
class StorageTransport {
public:
    using BoolCb = std::function<void(bool ok, const QString& error)>;

    virtual ~StorageTransport() = default;
    virtual void initAndStart(const QString& dataDir, BoolCb cb) = 0;
    virtual void ping(BoolCb cb) = 0;                          // node alive?
    virtual void fetch(const QString& cid, BoolCb cb) = 0;     // replicate to the local node
    virtual void removeCid(const QString& cid, BoolCb cb) = 0;
    virtual void exists(const QString& cid, std::function<void(bool ok, bool held)> cb) = 0;
    virtual void space(std::function<void(bool ok, qint64 usedBytes)> cb) = 0;
};

// Degradation fallback: used when the real transport cannot even be constructed
// (SDK/host ABI mismatch — see docs/spikes/uihost-getclient-abi-crash.md). The module
// must load and read channels regardless; storage simply reports offline.
class NullStorageTransport : public StorageTransport {
public:
    void initAndStart(const QString&, BoolCb cb) override
    {
        cb(false, QStringLiteral("storage_unavailable"));
    }
    void ping(BoolCb cb) override { cb(false, QStringLiteral("storage_unavailable")); }
    void fetch(const QString&, BoolCb cb) override
    {
        cb(false, QStringLiteral("storage_unavailable"));
    }
    void removeCid(const QString&, BoolCb cb) override
    {
        cb(false, QStringLiteral("storage_unavailable"));
    }
    void exists(const QString&, std::function<void(bool, bool)> cb) override
    {
        cb(false, false);
    }
    void space(std::function<void(bool, qint64)> cb) override { cb(false, 0); }
};

// Same signal surface as the P2 client — the plugin wiring and .rep semantics are
// unchanged; only where the bytes go changed.
class StorageClient : public QObject {
    Q_OBJECT
public:
    explicit StorageClient(StorageTransport* transport, QObject* parent = nullptr);

    // Async bring-up (storage_module start can block ~30s server-side; this never
    // blocks the caller). healthChanged fires when the node is up or failed.
    // If another module (stash) already started storage_module, ping short-circuits.
    void initStorage(const QString& dataDir);

    void pollHealth();
    QString storageState() const { return m_storageState; }

    void pin(const QString& cid);               // → pinFinished
    void unpin(const QString& cid);             // → unpinFinished
    void queryPinned(const QString& cid);       // → pinnedResult
    void queryRepoStat();                       // → repoStatResult(usedBytes)

    bool isPinning(const QString& cid) const { return m_pinning.contains(cid); }

signals:
    void healthChanged(const QString& state);
    void pinProgress(const QString& cid, qint64 blocks);   // reserved for storage events
    void pinFinished(const QString& cid, bool ok, const QString& error);
    void unpinFinished(const QString& cid, bool ok, const QString& error);
    void pinnedResult(const QString& cid, bool pinned);
    void repoStatResult(qint64 usedBytes);

private:
    void setState(bool up);

    StorageTransport* m_transport;   // owned by the plugin, outlives this client
    QString m_storageState = QStringLiteral("offline");
    QSet<QString> m_pinning;         // reentrancy guard per cid
    bool m_initInFlight = false;
};

#endif // STORAGE_CLIENT_H
