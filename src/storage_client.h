#ifndef STORAGE_CLIENT_H
#define STORAGE_CLIENT_H

#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Preserve side of the module (SPEC §5): pins a collection's CID on a Storage node over the
// Kubo-compatible HTTP RPC (the same API stash's gateway nodes serve):
//   POST /api/v0/version              → health (storageState)
//   POST /api/v0/pin/add?arg=<cid>&progress=true   → pin; streams {"Progress":n} JSON lines
//   POST /api/v0/pin/rm?arg=<cid>     → unpin
//   POST /api/v0/pin/ls?arg=<cid>     → pinned? (500 + "not pinned" when absent)
//   POST /api/v0/repo/stat            → {"RepoSize":bytes} for the summary counter
// `delegate` mode points this at the gateway's storageUrl; `local` mode at the user's own
// node — same client, different endpoint. All async; results land via signals.
class StorageClient : public QObject {
    Q_OBJECT
public:
    static constexpr int kHttpTimeoutMs = 15000;
    static constexpr qint64 kMaxBodyBytes = 1024 * 1024;

    explicit StorageClient(QObject* parent = nullptr);

    void setEndpoint(const QString& baseUrl);   // e.g. http://127.0.0.1:5001
    QString endpoint() const { return m_endpoint; }

    // delegate → the active gateway's storageUrl; local → the user's node
    static QString resolveEndpoint(const QString& mode, const QString& gatewayStorageUrl,
                                   const QString& localUrl);

    void pollHealth();                          // → healthChanged("ready"|"offline")
    QString storageState() const { return m_storageState; }

    void pin(const QString& cid);               // → pinProgress*, pinFinished
    void unpin(const QString& cid);             // → unpinFinished
    void queryPinned(const QString& cid);       // → pinnedResult
    void queryRepoStat();                       // → repoStatResult(usedBytes)

    bool isPinning(const QString& cid) const { return m_pinning.contains(cid); }

signals:
    void healthChanged(const QString& state);
    void pinProgress(const QString& cid, qint64 blocks);
    void pinFinished(const QString& cid, bool ok, const QString& error);
    void unpinFinished(const QString& cid, bool ok, const QString& error);
    void pinnedResult(const QString& cid, bool pinned);
    void repoStatResult(qint64 usedBytes);

private:
    QNetworkReply* httpPost(const QString& path, const QString& query = QString());
    void guardBodySize(QNetworkReply* reply);

    QNetworkAccessManager* m_net = nullptr;
    QString m_endpoint = QStringLiteral("http://127.0.0.1:5001");
    QString m_storageState = QStringLiteral("offline");
    QSet<QString> m_pinning;    // reentrancy guard per cid
};

#endif // STORAGE_CLIENT_H
