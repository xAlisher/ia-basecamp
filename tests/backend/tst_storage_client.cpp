#include <QSignalSpy>
#include <QtTest>

#include "storage_client.h"

// P2v2 backend tests per SPEC §13: preserve goes to Logos Storage via storage_module;
// the typed SDK sits behind StorageTransport, so tests run headless with a mock that
// records calls and fires deterministic callbacks.

class MockTransport : public StorageTransport {
public:
    QStringList calls;
    bool nodeUp = true;
    bool fetchOk = true;
    QString fetchError = QStringLiteral("fetch_failed");
    bool removeOk = true;
    QString removeError = QStringLiteral("remove_failed");
    QSet<QString> held;
    qint64 usedBytes = 0;
    bool deferInit = false;          // hold the init callback for manual release
    bool alreadyRunning = false;     // pre-existing instance answered version()
    BoolCb pendingInitCb;
    std::function<void(bool)> startedCb;   // the storageStart event hook

    void subscribeStarted(std::function<void(bool)> cb) override { startedCb = cb; }
    std::function<void(bool, const QString&, const QString&)> uploadDoneCb;
    bool uploadAccepted = true;
    void subscribeUploadDone(std::function<void(bool, const QString&, const QString&)> cb) override
    {
        uploadDoneCb = cb;
    }
    void upload(const QString& path, BoolCb cb) override
    {
        calls << "upload:" + path;
        cb(uploadAccepted, uploadAccepted ? QString() : QStringLiteral("upload_rejected"));
    }
    void initAndStart(const QString& dataDir, BoolCb cb) override
    {
        calls << "initAndStart:" + dataDir;
        if (deferInit) { pendingInitCb = cb; return; }
        if (!nodeUp) { cb(false, QStringLiteral("storage_start_failed")); return; }
        cb(true, alreadyRunning ? QStringLiteral("already_running") : QString());
    }
    void ping(BoolCb cb) override
    {
        calls << "ping";
        cb(nodeUp, nodeUp ? QString() : QStringLiteral("storage_unreachable"));
    }
    void fetch(const QString& cid, BoolCb cb) override
    {
        calls << "fetch:" + cid;
        if (fetchOk) held.insert(cid);
        cb(fetchOk, fetchOk ? QString() : fetchError);
    }
    void removeCid(const QString& cid, BoolCb cb) override
    {
        calls << "remove:" + cid;
        if (removeOk) held.remove(cid);
        cb(removeOk, removeOk ? QString() : removeError);
    }
    void exists(const QString& cid, std::function<void(bool, bool)> cb) override
    {
        calls << "exists:" + cid;
        cb(true, held.contains(cid));
    }
    void space(std::function<void(bool, qint64)> cb) override
    {
        calls << "space";
        cb(true, usedBytes);
    }
};

class TstStorageClient : public QObject {
    Q_OBJECT

private:
    MockTransport m_transport;

private slots:
    void init() { m_transport = MockTransport(); }

    void initStorage_twoPhaseReadiness()
    {
        // start() accepted → "starting" (yellow); storageStart event → "ready" (green)
        StorageClient c(&m_transport);
        QSignalSpy spy(&c, &StorageClient::healthChanged);
        c.initStorage(QStringLiteral("/tmp/x"));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("starting"));
        QVERIFY(m_transport.startedCb != nullptr);   // subscribed BEFORE init returned
        m_transport.startedCb(true);
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("ready"));
        QVERIFY(m_transport.calls.first().startsWith("initAndStart:/tmp/x"));
    }

    void initStorage_preExistingInstanceIsReady()
    {
        m_transport.alreadyRunning = true;
        StorageClient c(&m_transport);
        QSignalSpy spy(&c, &StorageClient::healthChanged);
        c.initStorage(QStringLiteral("/tmp/x"));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("ready"));
    }

    void initStorage_failureIsOffline()
    {
        m_transport.nodeUp = false;
        StorageClient c(&m_transport);
        QSignalSpy spy(&c, &StorageClient::healthChanged);
        c.initStorage(QStringLiteral("/tmp/x"));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("offline"));
    }

    void initStorage_reentrancyGuarded()
    {
        m_transport.deferInit = true;
        StorageClient c(&m_transport);
        c.initStorage(QStringLiteral("/tmp/x"));
        c.initStorage(QStringLiteral("/tmp/x"));     // ignored while in flight
        c.pollHealth();                              // also ignored while in flight
        QCOMPARE(m_transport.calls.size(), 1);
        QSignalSpy spy(&c, &StorageClient::healthChanged);
        m_transport.pendingInitCb(true, QString());  // release
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("starting"));
    }

    void health_pingNeverPromotesToReady()
    {
        StorageClient c(&m_transport);
        QSignalSpy spy(&c, &StorageClient::healthChanged);
        c.pollHealth();   // alive from offline → starting, NOT ready (event's job)
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("starting"));
        m_transport.nodeUp = false;
        c.pollHealth();
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("offline"));
    }

    void health_pingKeepsReadyOnceEventArrived()
    {
        StorageClient c(&m_transport);
        c.initStorage(QStringLiteral("/tmp/x"));
        m_transport.startedCb(true);
        QSignalSpy spy(&c, &StorageClient::healthChanged);
        c.pollHealth();
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("ready"));
    }

    void pin_success()
    {
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::pinFinished);
        c.pin(QStringLiteral("zDvZ-cid-1"));
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.last().at(0).toString(), QStringLiteral("zDvZ-cid-1"));
        QCOMPARE(done.last().at(1).toBool(), true);
        QVERIFY(m_transport.held.contains("zDvZ-cid-1"));
        QCOMPARE(m_transport.calls.last(), QStringLiteral("fetch:zDvZ-cid-1"));
    }

    void pin_failureSurfacesError()
    {
        m_transport.fetchOk = false;
        m_transport.fetchError = QStringLiteral("dataset not found on network");
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::pinFinished);
        c.pin(QStringLiteral("cid-1"));
        QCOMPARE(done.last().at(1).toBool(), false);
        QCOMPARE(done.last().at(2).toString(), QStringLiteral("dataset not found on network"));
    }

    void pin_reentrancyGuarded()
    {
        // a transport that never completes — second pin of the same cid must be rejected
        struct HangingTransport : MockTransport {
            void fetch(const QString& cid, BoolCb) override { calls << "fetch:" + cid; }
        } hanging;
        StorageClient c(&hanging);
        QSignalSpy done(&c, &StorageClient::pinFinished);
        c.pin(QStringLiteral("cid-1"));
        c.pin(QStringLiteral("cid-1"));
        QCOMPARE(done.count(), 1);   // only the guard rejection fired
        QCOMPARE(done.last().at(2).toString(), QStringLiteral("pin_in_progress"));
        QCOMPARE(hanging.calls.count("fetch:cid-1"), 1);
    }

    void unpin_success_andNotFoundIsSuccess()
    {
        m_transport.held.insert("cid-1");
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::unpinFinished);
        c.unpin(QStringLiteral("cid-1"));
        QCOMPARE(done.last().at(1).toBool(), true);
        QVERIFY(!m_transport.held.contains("cid-1"));

        // already gone — storage says "not found"; unmirror's end state is identical
        m_transport.removeOk = false;
        m_transport.removeError = QStringLiteral("dataset Not Found");
        c.unpin(QStringLiteral("cid-1"));
        QCOMPARE(done.last().at(1).toBool(), true);
    }

    void unpin_realFailureSurfaces()
    {
        m_transport.removeOk = false;
        m_transport.removeError = QStringLiteral("storage corrupt");
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::unpinFinished);
        c.unpin(QStringLiteral("cid-1"));
        QCOMPARE(done.last().at(1).toBool(), false);
        QCOMPARE(done.last().at(2).toString(), QStringLiteral("storage corrupt"));
    }

    void queryPinned_bothWays()
    {
        m_transport.held.insert("cid-1");
        StorageClient c(&m_transport);
        QSignalSpy res(&c, &StorageClient::pinnedResult);
        c.queryPinned(QStringLiteral("cid-1"));
        QCOMPARE(res.last().at(1).toBool(), true);
        c.queryPinned(QStringLiteral("cid-other"));
        QCOMPARE(res.last().at(1).toBool(), false);
    }

    void upload_completesViaEvent()
    {
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::uploadFinished);
        c.upload(QStringLiteral("/tmp/f.bin"));
        QCOMPARE(done.count(), 0);                       // accepted — waiting on the event
        m_transport.uploadDoneCb(true, QStringLiteral("zDvZNewCid"), QString());
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.last().at(0).toBool(), true);
        QCOMPARE(done.last().at(1).toString(), QStringLiteral("zDvZNewCid"));
        // a second upload works after completion
        c.upload(QStringLiteral("/tmp/g.bin"));
        QCOMPARE(m_transport.calls.count("upload:/tmp/g.bin"), 1);
    }

    void upload_singleInFlight()
    {
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::uploadFinished);
        c.upload(QStringLiteral("/tmp/a"));
        c.upload(QStringLiteral("/tmp/b"));              // rejected by the guard
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.last().at(2).toString(), QStringLiteral("upload_in_progress"));
        QCOMPARE(m_transport.calls.count("upload:/tmp/b"), 0);
    }

    void upload_rejectionFailsImmediately()
    {
        m_transport.uploadAccepted = false;
        StorageClient c(&m_transport);
        QSignalSpy done(&c, &StorageClient::uploadFinished);
        c.upload(QStringLiteral("/tmp/a"));
        QCOMPARE(done.last().at(0).toBool(), false);
        QCOMPARE(done.last().at(2).toString(), QStringLiteral("upload_rejected"));
    }

    void repoStat_reportsUsedBytes()
    {
        m_transport.usedBytes = 123456;
        StorageClient c(&m_transport);
        QSignalSpy res(&c, &StorageClient::repoStatResult);
        c.queryRepoStat();
        QCOMPARE(res.last().at(0).toLongLong(), qint64(123456));
    }
};

QTEST_MAIN(TstStorageClient)
#include "tst_storage_client.moc"
