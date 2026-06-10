#include "storage_client.h"

StorageClient::StorageClient(StorageTransport* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
}

void StorageClient::setState(const QString& state)
{
    m_storageState = state;
    emit healthChanged(m_storageState);
}

void StorageClient::initStorage(const QString& dataDir)
{
    if (m_initInFlight)
        return;
    m_initInFlight = true;
    // subscribe BEFORE any IPC — the storageStart event is the real "ready for
    // transfers" signal (start() returning only means bootstrap began)
    m_transport->subscribeStarted([this](bool ok) {
        setState(ok ? QStringLiteral("ready") : QStringLiteral("offline"));
    });
    m_transport->initAndStart(dataDir, [this](bool ok, const QString& error) {
        m_initInFlight = false;
        if (!ok) {
            setState(QStringLiteral("offline"));
            return;
        }
        // a pre-existing instance already went through its bootstrap
        setState(error == QLatin1String("already_running") ? QStringLiteral("ready")
                                                           : QStringLiteral("starting"));
    });
}

void StorageClient::pollHealth()
{
    if (m_initInFlight)
        return;   // bring-up in progress — its completion sets the state
    m_transport->ping([this](bool ok, const QString&) {
        if (!ok) {
            setState(QStringLiteral("offline"));
            return;
        }
        // never promote to ready from a mere ping — that's the event's job;
        // recovering from offline lands in starting until storageStart says green
        if (m_storageState == QLatin1String("offline"))
            setState(QStringLiteral("starting"));
        else
            emit healthChanged(m_storageState);   // unchanged, keep the UI in sync
    });
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

void StorageClient::upload(const QString& path)
{
    if (m_uploading) {
        emit uploadFinished(false, QString(), QStringLiteral("upload_in_progress"));
        return;
    }
    m_uploading = true;
    if (!m_uploadSubscribed) {
        m_uploadSubscribed = true;
        m_transport->subscribeUploadDone(
            [this](bool ok, const QString& cid, const QString& error) {
                if (!m_uploading)
                    return;   // stray event from another consumer
                m_uploading = false;
                emit uploadFinished(ok, cid, error);
            });
    }
    m_transport->upload(path, [this](bool accepted, const QString& error) {
        if (!accepted) {
            m_uploading = false;
            emit uploadFinished(false, QString(), error);
        }
        // accepted → completion arrives via the storageUploadDone event
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
