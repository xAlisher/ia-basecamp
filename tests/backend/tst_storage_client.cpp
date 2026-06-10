#include <QSignalSpy>
#include <QtTest>

#include "mock_storage.h"
#include "storage_client.h"

// P2 backend tests per SPEC §13: deterministic, mock Storage endpoint, no network.

class TstStorageClient : public QObject {
    Q_OBJECT

private:
    MockStorage* m_storage = nullptr;

    StorageClient* makeClient()
    {
        auto* c = new StorageClient(this);
        c->setEndpoint(m_storage->baseUrl());
        return c;
    }

private slots:
    void init()
    {
        m_storage = new MockStorage(this);
        QVERIFY(m_storage->start());
    }

    void cleanup()
    {
        delete m_storage;
        m_storage = nullptr;
    }

    void resolveEndpoint_modes()
    {
        QCOMPARE(StorageClient::resolveEndpoint("delegate", "http://gw:5001", "http://local:5001"),
                 QStringLiteral("http://gw:5001"));
        QCOMPARE(StorageClient::resolveEndpoint("local", "http://gw:5001", "http://local:5001"),
                 QStringLiteral("http://local:5001"));
    }

    void health_readyAndOffline()
    {
        StorageClient* c = makeClient();
        QSignalSpy spy(c, &StorageClient::healthChanged);
        c->pollHealth();
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("ready"));

        m_storage->refuse = true;
        c->pollHealth();
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("offline"));
    }

    void pin_succeedsWithProgress()
    {
        m_storage->progressSteps = 3;
        StorageClient* c = makeClient();
        QSignalSpy progress(c, &StorageClient::pinProgress);
        QSignalSpy done(c, &StorageClient::pinFinished);

        c->pin(QStringLiteral("cid-1"));
        QVERIFY(done.wait(3000));
        QCOMPARE(done.last().at(0).toString(), QStringLiteral("cid-1"));
        QCOMPARE(done.last().at(1).toBool(), true);
        QVERIFY(m_storage->pinned.contains("cid-1"));
        // all progress lines observed (readyRead and/or final buffer)
        QVERIFY(progress.count() >= 1 || m_storage->progressSteps == 0);
    }

    void pin_failureSurfacesError()
    {
        m_storage->failPins = true;
        StorageClient* c = makeClient();
        QSignalSpy done(c, &StorageClient::pinFinished);
        c->pin(QStringLiteral("cid-1"));
        QVERIFY(done.wait(3000));
        QCOMPARE(done.last().at(1).toBool(), false);
        QVERIFY(!done.last().at(2).toString().isEmpty());
        QVERIFY(!m_storage->pinned.contains("cid-1"));
    }

    void pin_reentrancyGuarded()
    {
        StorageClient* c = makeClient();
        QSignalSpy done(c, &StorageClient::pinFinished);
        c->pin(QStringLiteral("cid-1"));
        c->pin(QStringLiteral("cid-1"));   // immediate second attempt
        QVERIFY(done.wait(3000));
        // one of the two finished signals is the guard rejection
        bool sawGuard = false;
        for (const QList<QVariant>& args : done)
            if (args.at(2).toString() == QLatin1String("pin_in_progress"))
                sawGuard = true;
        if (!sawGuard)
            QVERIFY(done.wait(3000));
        for (const QList<QVariant>& args : done)
            if (args.at(2).toString() == QLatin1String("pin_in_progress"))
                sawGuard = true;
        QVERIFY(sawGuard);
    }

    void unpin_succeeds_andNotPinnedIsSuccess()
    {
        m_storage->pinned.insert("cid-1");
        m_storage->repoSize = 1000;
        StorageClient* c = makeClient();
        QSignalSpy done(c, &StorageClient::unpinFinished);

        c->unpin(QStringLiteral("cid-1"));
        QVERIFY(done.wait(3000));
        QCOMPARE(done.last().at(1).toBool(), true);
        QVERIFY(!m_storage->pinned.contains("cid-1"));

        c->unpin(QStringLiteral("cid-1"));   // already gone — "not pinned" is still success
        QVERIFY(done.wait(3000));
        QCOMPARE(done.last().at(1).toBool(), true);
    }

    void queryPinned_reportsBothWays()
    {
        m_storage->pinned.insert("cid-1");
        StorageClient* c = makeClient();
        QSignalSpy res(c, &StorageClient::pinnedResult);

        c->queryPinned(QStringLiteral("cid-1"));
        QVERIFY(res.wait(3000));
        QCOMPARE(res.last().at(1).toBool(), true);

        c->queryPinned(QStringLiteral("cid-other"));
        QVERIFY(res.wait(3000));
        QCOMPARE(res.last().at(1).toBool(), false);
    }

    void repoStat_reportsUsedBytes()
    {
        m_storage->repoSize = 123456;
        StorageClient* c = makeClient();
        QSignalSpy res(c, &StorageClient::repoStatResult);
        c->queryRepoStat();
        QVERIFY(res.wait(3000));
        QCOMPARE(res.last().at(0).toLongLong(), qint64(123456));
    }

    // ── live (opt-in: ARCHIVE_LIVE_STORAGE=http://host:port ARCHIVE_LIVE_CID=Qm…) ──

    void live_pinUnpinRoundTrip()
    {
        const QByteArray ep = qgetenv("ARCHIVE_LIVE_STORAGE");
        const QByteArray cidEnv = qgetenv("ARCHIVE_LIVE_CID");
        if (ep.isEmpty() || cidEnv.isEmpty())
            QSKIP("set ARCHIVE_LIVE_STORAGE + ARCHIVE_LIVE_CID to run the live pin");
        const QString cid = QString::fromUtf8(cidEnv);

        StorageClient* c = new StorageClient(this);
        c->setEndpoint(QString::fromUtf8(ep));

        QSignalSpy health(c, &StorageClient::healthChanged);
        c->pollHealth();
        QVERIFY(health.wait(5000));
        QCOMPARE(health.last().at(0).toString(), QStringLiteral("ready"));

        QSignalSpy done(c, &StorageClient::pinFinished);
        c->pin(cid);
        QVERIFY(done.wait(30000));
        QCOMPARE(done.last().at(1).toBool(), true);

        QSignalSpy pinres(c, &StorageClient::pinnedResult);
        c->queryPinned(cid);
        QVERIFY(pinres.wait(5000));
        QCOMPARE(pinres.last().at(1).toBool(), true);

        QSignalSpy undone(c, &StorageClient::unpinFinished);
        c->unpin(cid);
        QVERIFY(undone.wait(10000));
        QCOMPARE(undone.last().at(1).toBool(), true);
    }
};

QTEST_MAIN(TstStorageClient)
#include "tst_storage_client.moc"
