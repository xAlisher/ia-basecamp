#include "storage_module_api.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

// ModuleProxy on the storage_module side converts LogosResult → JSON string before
// sending over QRO, because LogosResult is not Q_DECLARE_METATYPE-d and cannot
// be serialised across process boundaries.  The JSON arrives as QVariant(QString).
// parseLogosResult() handles both the (broken) LogosResult QVariant and the JSON
// string path so all upload/download helpers return meaningful values.
static LogosResult parseLogosResult(const QVariant& v) {
    if (!v.isValid()) return {};
    // Try direct cast first (works when both sides share the same metatype id).
    if (v.canConvert<LogosResult>()) {
        LogosResult r = qvariant_cast<LogosResult>(v);
        if (r.success || !r.error.toString().isEmpty()) return r;
    }
    // Fall back to JSON string path (the common cross-process case).
    const QString s = v.toString();
    if (s.isEmpty()) return {};
    const QJsonObject obj = QJsonDocument::fromJson(s.toUtf8()).object();
    if (obj.isEmpty()) return {};
    LogosResult r;
    r.success = obj[QStringLiteral("success")].toBool();
    r.value   = obj[QStringLiteral("value")].toVariant();
    r.error   = obj[QStringLiteral("error")].toVariant();
    return r;
}

StorageModule::StorageModule(LogosAPI* api) : m_api(api), m_client(api->getClient("storage_module")), m_moduleName(QStringLiteral("storage_module")) {}

LogosObject* StorageModule::ensureReplica() {
    if (!m_eventReplica) {
        LogosObject* replica = m_client->requestObject(m_moduleName);
        if (!replica) {
            qWarning() << "StorageModule: failed to acquire remote object for events on" << m_moduleName;
            return nullptr;
        }
        m_eventReplica = replica;
    }
    return m_eventReplica;
}

bool StorageModule::on(const QString& eventName, RawEventCallback callback) {
    if (!callback) {
        qWarning() << "StorageModule: ignoring empty event callback for" << eventName;
        return false;
    }
    LogosObject* origin = ensureReplica();
    if (!origin) {
        return false;
    }
    m_client->onEvent(origin, eventName, callback);
    return true;
}

bool StorageModule::on(const QString& eventName, EventCallback callback) {
    if (!callback) {
        qWarning() << "StorageModule: ignoring empty event callback for" << eventName;
        return false;
    }
    return on(eventName, [callback](const QString&, const QVariantList& data) {
        callback(data);
    });
}

void StorageModule::setEventSource(LogosObject* source) {
    m_eventSource = source;
}

LogosObject* StorageModule::eventSource() const {
    return m_eventSource;
}

void StorageModule::trigger(const QString& eventName) {
    trigger(eventName, QVariantList{});
}

void StorageModule::trigger(const QString& eventName, const QVariantList& data) {
    if (!m_eventSource) {
        qWarning() << "StorageModule: no event source set for trigger" << eventName;
        return;
    }
    m_client->onEventResponse(m_eventSource, eventName, data);
}

void StorageModule::trigger(const QString& eventName, LogosObject* source, const QVariantList& data) {
    if (!source) {
        qWarning() << "StorageModule: cannot trigger" << eventName << "with null source";
        return;
    }
    m_client->onEventResponse(source, eventName, data);
}

bool StorageModule::init(const QString& cfg) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "init", cfg);
    return _result.toBool();
}

void StorageModule::initAsync(const QString& cfg, std::function<void(bool)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "init", QVariantList() << cfg, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<bool>(v) : false);
    }, timeout);
}

bool StorageModule::start() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "start");
    return _result.toBool();
}

void StorageModule::startAsync(std::function<void(bool)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "start", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<bool>(v) : false);
    }, timeout);
}

LogosResult StorageModule::stop() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "stop");
    return _result.value<LogosResult>();
}

void StorageModule::stopAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "stop", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::destroy() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "destroy");
    return _result.value<LogosResult>();
}

void StorageModule::destroyAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "destroy", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::version() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "version");
    return _result.value<LogosResult>();
}

void StorageModule::versionAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "version", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::dataDir() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "dataDir");
    return _result.value<LogosResult>();
}

void StorageModule::dataDirAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "dataDir", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::peerId() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "peerId");
    return _result.value<LogosResult>();
}

void StorageModule::peerIdAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "peerId", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::spr() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "spr");
    return _result.value<LogosResult>();
}

void StorageModule::sprAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "spr", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::debug() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "debug");
    return _result.value<LogosResult>();
}

void StorageModule::debugAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "debug", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::updateLogLevel(const QString& logLevel) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "updateLogLevel", logLevel);
    return _result.value<LogosResult>();
}

void StorageModule::updateLogLevelAsync(const QString& logLevel, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "updateLogLevel", QVariantList() << logLevel, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::connect(const QString& peerId, const QStringList& peerAddresses) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "connect", peerId, peerAddresses);
    return _result.value<LogosResult>();
}

void StorageModule::connectAsync(const QString& peerId, const QStringList& peerAddresses, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "connect", QVariantList{peerId, peerAddresses}, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::uploadUrl(const QString& filePath, int chunkSize) {
    // Pass as QUrl so the server-side StorageModuleProviderObject::uploadUrl(QUrl, int)
    // dispatch path is hit — the QString path goes through QUrl::fromLocalFile which
    // produces a proper file:// URL that the server converts back via toLocalFile().
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "uploadUrl",
        QVariant::fromValue(QUrl::fromLocalFile(filePath)), chunkSize);
    return parseLogosResult(_result);
}

void StorageModule::uploadUrlAsync(const QString& filePath, int chunkSize, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "uploadUrl",
        QVariantList{QVariant::fromValue(QUrl::fromLocalFile(filePath)), chunkSize},
        [callback](QVariant v) {
            callback(parseLogosResult(v));
        }, timeout);
}

LogosResult StorageModule::uploadInit(const QString& filename, int chunkSize) {
    // The first uploadInit call after storage start() can take ~20 s while the Go
    // node finishes internal initialization.  Use a 60 s timeout to outlast it.
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "uploadInit",
        QVariant(filename), QVariant(chunkSize), Timeout(60000));
    return parseLogosResult(_result);
}

void StorageModule::uploadInitAsync(const QString& filename, int chunkSize, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "uploadInit", QVariantList{filename, chunkSize}, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::uploadChunk(const QString& sessionId, const QString& chunk) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "uploadChunk",
        QVariant(sessionId), QVariant(chunk), Timeout(60000));
    return parseLogosResult(_result);
}

void StorageModule::uploadChunkAsync(const QString& sessionId, const QString& chunk, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "uploadChunk", QVariantList{sessionId, chunk}, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::uploadFinalize(const QString& sessionId) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "uploadFinalize",
        QVariant(sessionId), Timeout(60000));
    return parseLogosResult(_result);
}

void StorageModule::uploadFinalizeAsync(const QString& sessionId, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "uploadFinalize", QVariantList() << sessionId, [callback](QVariant v) {
        callback(parseLogosResult(v));
    }, timeout);
}

LogosResult StorageModule::uploadCancel(const QString& sessionId) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "uploadCancel", sessionId);
    return _result.value<LogosResult>();
}

void StorageModule::uploadCancelAsync(const QString& sessionId, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "uploadCancel", QVariantList() << sessionId, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::downloadChunks(const QString& cid, bool local, int chunkSize) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "downloadChunks", cid, local, chunkSize);
    return _result.value<LogosResult>();
}

void StorageModule::downloadChunksAsync(const QString& cid, bool local, int chunkSize, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "downloadChunks", QVariantList{cid, local, chunkSize}, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::downloadCancel(const QString& sessionId) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "downloadCancel", sessionId);
    return _result.value<LogosResult>();
}

void StorageModule::downloadCancelAsync(const QString& sessionId, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "downloadCancel", QVariantList() << sessionId, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::exists(const QString& cid) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "exists", cid);
    return _result.value<LogosResult>();
}

void StorageModule::existsAsync(const QString& cid, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "exists", QVariantList() << cid, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::fetch(const QString& cid) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "fetch", cid);
    return _result.value<LogosResult>();
}

void StorageModule::fetchAsync(const QString& cid, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "fetch", QVariantList() << cid, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::remove(const QString& cid) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "remove", cid);
    return _result.value<LogosResult>();
}

void StorageModule::removeAsync(const QString& cid, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "remove", QVariantList() << cid, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::space() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "space");
    return _result.value<LogosResult>();
}

void StorageModule::spaceAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "space", QVariantList(), [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

LogosResult StorageModule::manifests() {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "manifests");
    return _result.value<LogosResult>();
}

void StorageModule::manifestsAsync(std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "manifests", QVariantList(), [callback](QVariant v) {
        callback(parseLogosResult(v));
    }, timeout);
}

LogosResult StorageModule::downloadManifest(const QString& cid) {
    QVariant _result = m_client->invokeRemoteMethod("storage_module", "downloadManifest", cid);
    return _result.value<LogosResult>();
}

void StorageModule::downloadManifestAsync(const QString& cid, std::function<void(LogosResult)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "downloadManifest", QVariantList() << cid, [callback](QVariant v) {
        callback(v.isValid() ? qvariant_cast<LogosResult>(v) : LogosResult{});
    }, timeout);
}

void StorageModule::importFiles(const QString& path) {
    m_client->invokeRemoteMethod("storage_module", "importFiles", path);
}

void StorageModule::importFilesAsync(const QString& path, std::function<void(void)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "importFiles", QVariantList() << path, [callback](QVariant v) {
        callback();
    }, timeout);
}

void StorageModule::emitEventSafe(const QString& name, const QString& data) {
    m_client->invokeRemoteMethod("storage_module", "emitEventSafe", name, data);
}

void StorageModule::emitEventSafeAsync(const QString& name, const QString& data, std::function<void(void)> callback, Timeout timeout) {
    if (!callback) return;
    m_client->invokeRemoteMethodAsync("storage_module", "emitEventSafe", QVariantList{name, data}, [callback](QVariant v) {
        callback();
    }, timeout);
}

