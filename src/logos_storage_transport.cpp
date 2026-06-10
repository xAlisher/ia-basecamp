#include "logos_storage_transport.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include "logos_api.h"
#include "storage_module_api.h"

namespace {

QString errorOf(const LogosResult& r)
{
    const QString e = r.error.toString();
    return e.isEmpty() ? QStringLiteral("storage_error") : e;
}

// LogosResult values arrive as strings whose shape isn't pinned by the API docs —
// decode defensively (plain number, bool, or a JSON object with a used-bytes field).
qint64 usedBytesFrom(const LogosResult& r)
{
    QString v;
    try {
        v = r.getValue<QString>();
    } catch (...) {
        return -1;
    }
    bool numOk = false;
    const qint64 n = v.toLongLong(&numOk);
    if (numOk)
        return n;
    const QJsonObject o = QJsonDocument::fromJson(v.toUtf8()).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        if (it.key().contains(QLatin1String("used"), Qt::CaseInsensitive)
            && it.value().isDouble())
            return static_cast<qint64>(it.value().toDouble());
    }
    return -1;
}

bool boolFrom(const LogosResult& r)
{
    try {
        const QString v = r.getValue<QString>();
        return v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
               || v == QLatin1String("1");
    } catch (...) {
        return false;
    }
}

} // namespace

// All calls are SYNCHRONOUS typed-SDK calls (invokeRemoteMethod under the hood, 20s
// timeout) — exactly stash's style. The *Async variants of this SDK era do not deliver
// results reliably; stash never uses them. Callbacks fire before these methods return.

LogosStorageTransport::LogosStorageTransport(LogosAPI* api)
    : m_storage(new StorageModule(api))
{
}

LogosStorageTransport::~LogosStorageTransport()
{
    delete m_storage;
}

void LogosStorageTransport::subscribeStarted(std::function<void(bool)> cb)
{
    // storageStart payload: [bool ok, msg] (stash's documented shape)
    m_storage->on(QStringLiteral("storageStart"), [cb](const QVariantList& d) {
        cb(!d.isEmpty() && d.first().toBool());
    });
}

void LogosStorageTransport::ping(BoolCb cb)
{
    const LogosResult r = m_storage->version();
    cb(r.success, r.success ? QString() : errorOf(r));
}

void LogosStorageTransport::initAndStart(const QString& dataDir, BoolCb cb)
{
    // stash's sequence verbatim: init({data-dir}) then start(); no probing.
    // storage_module is shared — if another module already inited/started it,
    // a failed init with a live version() still counts as up.
    QDir().mkpath(dataDir);
    const QJsonObject cfg{ { QStringLiteral("data-dir"), dataDir } };
    const QString cfgJson =
        QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));

    const bool initOk = m_storage->init(cfgJson);
    if (initOk) {
        const bool started = m_storage->start();
        if (started) {
            cb(true, QString());
            return;
        }
    }
    const LogosResult alive = m_storage->version();
    if (alive.success) {
        // someone else (stash / a previous session) runs it — bootstrap already done
        cb(true, QStringLiteral("already_running"));
        return;
    }
    cb(false, initOk ? QStringLiteral("storage_start_failed")
                     : QStringLiteral("storage_init_failed"));
}

void LogosStorageTransport::subscribeUploadDone(
    std::function<void(bool, const QString&, const QString&)> cb)
{
    // storageUploadDone: the CID is the last non-empty string arg (stash's parsing)
    m_storage->on(QStringLiteral("storageUploadDone"), [cb](const QVariantList& d) {
        QString cid;
        for (auto it = d.rbegin(); it != d.rend(); ++it) {
            if (it->canConvert<QString>() && !it->toString().isEmpty()) {
                cid = it->toString();
                break;
            }
        }
        const bool ok = !d.isEmpty() && d.first().toBool() && !cid.isEmpty();
        cb(ok, cid, ok ? QString() : QStringLiteral("upload_failed"));
    });
}

void LogosStorageTransport::upload(const QString& path, BoolCb cb)
{
    constexpr int kLogosChunkSize = 65536;   // stash/keeper's chunking — reproduces CIDs
    const LogosResult r = m_storage->uploadUrl(path, kLogosChunkSize);
    cb(r.success, r.success ? QString() : errorOf(r));
}

void LogosStorageTransport::fetch(const QString& cid, BoolCb cb)
{
    const LogosResult r = m_storage->fetch(cid);
    cb(r.success, r.success ? QString() : errorOf(r));
}

void LogosStorageTransport::removeCid(const QString& cid, BoolCb cb)
{
    const LogosResult r = m_storage->remove(cid);
    cb(r.success, r.success ? QString() : errorOf(r));
}

void LogosStorageTransport::exists(const QString& cid, std::function<void(bool, bool)> cb)
{
    const LogosResult r = m_storage->exists(cid);
    cb(r.success, r.success && boolFrom(r));
}

void LogosStorageTransport::space(std::function<void(bool, qint64)> cb)
{
    const LogosResult r = m_storage->space();
    const qint64 bytes = r.success ? usedBytesFrom(r) : -1;
    cb(r.success && bytes >= 0, bytes < 0 ? 0 : bytes);
}
