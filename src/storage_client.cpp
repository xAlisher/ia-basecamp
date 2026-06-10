#include "storage_client.h"

StorageClient::StorageClient(StorageTransport* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
}

void StorageClient::setState(bool up)
{
    m_storageState = up ? QStringLiteral("ready") : QStringLiteral("offline");
    emit healthChanged(m_storageState);
}

void StorageClient::initStorage(const QString& dataDir)
{
    if (m_initInFlight)
        return;
    m_initInFlight = true;
    m_transport->initAndStart(dataDir, [this](bool ok, const QString&) {
        m_initInFlight = false;
        setState(ok);
    });
}

void StorageClient::pollHealth()
{
    if (m_initInFlight)
        return;   // bring-up in progress — its completion sets the state
    m_transport->ping([this](bool ok, const QString&) { setState(ok); });
}

void StorageClient::pin(const QString& cid)
{
    if (m_pinning.contains(cid)) {
        emit pinFinished(cid, false, QStringLiteral("pin_in_progress"));
        return;
    }
    m_pinning.insert(cid);
    m_transport->fetch(cid, [this, cid](bool ok, const QString& error) {
        m_pinning.remove(cid);
        emit pinFinished(cid, ok, ok ? QString() : error);
    });
}

void StorageClient::unpin(const QString& cid)
{
    m_transport->removeCid(cid, [this, cid](bool ok, const QString& error) {
        // "not held" is success for unmirror purposes — the end state is identical
        if (!ok && error.contains(QLatin1String("not found"), Qt::CaseInsensitive)) {
            emit unpinFinished(cid, true, QString());
            return;
        }
        emit unpinFinished(cid, ok, ok ? QString() : error);
    });
}

void StorageClient::queryPinned(const QString& cid)
{
    m_transport->exists(cid, [this, cid](bool ok, bool held) {
        emit pinnedResult(cid, ok && held);
    });
}

void StorageClient::queryRepoStat()
{
    m_transport->space([this](bool ok, qint64 usedBytes) {
        if (ok)
            emit repoStatResult(usedBytes);
    });
}
