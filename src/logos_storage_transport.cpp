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

LogosStorageTransport::LogosStorageTransport(LogosAPI* api)
    : m_storage(new StorageModule(api))
{
}

LogosStorageTransport::~LogosStorageTransport()
{
    delete m_storage;
}

void LogosStorageTransport::ping(BoolCb cb)
{
    m_storage->versionAsync([cb](LogosResult r) { cb(r.success, errorOf(r)); });
}

void LogosStorageTransport::initAndStart(const QString& dataDir, BoolCb cb)
{
    // storage_module is shared platform infrastructure — stash (or the platform itself)
    // may already have it running. Probe first; only init+start when it isn't up.
    m_storage->versionAsync([this, dataDir, cb](LogosResult probe) {
        if (probe.success) {
            cb(true, QString());
            return;
        }
        QDir().mkpath(dataDir);
        const QJsonObject cfg{ { QStringLiteral("data-dir"), dataDir } };
        const QString cfgJson =
            QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
        m_storage->initAsync(cfgJson, [this, cb](bool initOk) {
            if (!initOk) {
                cb(false, QStringLiteral("storage_init_failed"));
                return;
            }
            m_storage->startAsync([cb](bool startOk) {
                cb(startOk, startOk ? QString() : QStringLiteral("storage_start_failed"));
            });
        });
    });
}

void LogosStorageTransport::fetch(const QString& cid, BoolCb cb)
{
    m_storage->fetchAsync(cid, [cb](LogosResult r) { cb(r.success, errorOf(r)); });
}

void LogosStorageTransport::removeCid(const QString& cid, BoolCb cb)
{
    m_storage->removeAsync(cid, [cb](LogosResult r) { cb(r.success, errorOf(r)); });
}

void LogosStorageTransport::exists(const QString& cid, std::function<void(bool, bool)> cb)
{
    m_storage->existsAsync(cid, [cb](LogosResult r) { cb(r.success, boolFrom(r)); });
}

void LogosStorageTransport::space(std::function<void(bool, qint64)> cb)
{
    m_storage->spaceAsync([cb](LogosResult r) {
        const qint64 bytes = r.success ? usedBytesFrom(r) : -1;
        cb(r.success && bytes >= 0, bytes < 0 ? 0 : bytes);
    });
}
