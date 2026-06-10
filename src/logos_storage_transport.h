#ifndef LOGOS_STORAGE_TRANSPORT_H
#define LOGOS_STORAGE_TRANSPORT_H

#include "storage_client.h"

class LogosAPI;
class StorageModule;   // generated typed client (builder codegen from metadata dependencies)

// The real StorageTransport: Logos Storage via the platform storage_module, over the
// typed SDK — exactly stash's integration path. Compiled only into the plugin (the
// generated headers don't exist in the test build; tests use a mock transport).
class LogosStorageTransport : public StorageTransport {
public:
    explicit LogosStorageTransport(LogosAPI* api);
    ~LogosStorageTransport() override;

    void subscribeStarted(std::function<void(bool ok)> cb) override;
    void initAndStart(const QString& dataDir, BoolCb cb) override;
    void ping(BoolCb cb) override;
    void fetch(const QString& cid, BoolCb cb) override;
    void subscribeUploadDone(std::function<void(bool ok, const QString& cid,
                                                const QString& error)> cb) override;
    void upload(const QString& path, BoolCb cb) override;
    void removeCid(const QString& cid, BoolCb cb) override;
    void exists(const QString& cid, std::function<void(bool ok, bool held)> cb) override;
    void space(std::function<void(bool ok, qint64 usedBytes)> cb) override;

private:
    StorageModule* m_storage;   // owned
};

#endif // LOGOS_STORAGE_TRANSPORT_H
