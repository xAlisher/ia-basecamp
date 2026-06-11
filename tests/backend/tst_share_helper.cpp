#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "share_helper.h"

// P6 backend tests per SPEC §13: share data shape/totals + PNG actually written.

namespace {

QJsonObject col(const char* id, const char* title, qint64 sizeBytes, const char* state,
                const char* thumb = "")
{
    return { { "id", id },          { "title", title }, { "sizeBytes", sizeBytes },
             { "state", state },    { "thumbnail", thumb },
             { "txHash", QString("tx-%1").arg(id) } };
}

QString validPngBase64()
{
    QImage img(4, 4, QImage::Format_RGB32);
    img.fill(Qt::red);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QString::fromLatin1(bytes.toBase64());
}

} // namespace

class TstShareHelper : public QObject {
    Q_OBJECT

private slots:
    void shareData_me_totalsMirroredOnly()
    {
        const QJsonArray cols{
            col("c1", "Maps", 2000000000, "mirrored", "thumb-1"),
            col("c2", "Books", 1500000000, "mirrored"),
            col("c3", "Films", 9000000000, "available"),   // not preserved → excluded
        };
        const QJsonObject d = ShareHelper::buildShareData(cols, "me", "http://gw:5001");
        QVERIFY(d.value("ok").toBool());
        QCOMPARE(d.value("count").toInt(), 2);
        QCOMPARE(d.value("totalGB").toDouble(), 3.5);
        QCOMPARE(d.value("items").toArray().size(), 2);
        QCOMPARE(d.value("items").toArray()[0].toObject().value("name").toString(),
                 QStringLiteral("Maps"));
        QCOMPARE(d.value("items").toArray()[0].toObject().value("thumbnail").toString(),
                 QStringLiteral("http://gw:5001/ipfs/thumb-1"));
    }

    void shareData_me_emptyWhenNothingMirrored()
    {
        const QJsonArray cols{ col("c1", "Maps", 1, "available") };
        const QJsonObject d = ShareHelper::buildShareData(cols, "me");
        QCOMPARE(d.value("ok").toBool(), false);
        QCOMPARE(d.value("error").toString(), QStringLiteral("nothing_preserved_yet"));
    }

    void shareData_itemScope()
    {
        const QJsonArray cols{ col("c1", "Maps", 2000000000, "available") };
        const QJsonObject d = ShareHelper::buildShareData(cols, "c1");
        QVERIFY(d.value("ok").toBool());
        QCOMPARE(d.value("title").toString(), QStringLiteral("Maps"));
        QCOMPARE(d.value("totalGB").toDouble(), 2.0);
        QCOMPARE(d.value("txHash").toString(), QStringLiteral("tx-c1"));
        QCOMPARE(d.value("items").toArray().size(), 1);
    }

    void shareData_unknownItem()
    {
        const QJsonObject d = ShareHelper::buildShareData({}, "nope");
        QCOMPARE(d.value("ok").toBool(), false);
        QCOMPARE(d.value("error").toString(), QStringLiteral("unknown_item"));
    }

    void thumbnails_onlyCidShapedResolved()
    {
        // Senty P6 HIGH: chain-inscribed URLs must never reach Image.source
        const QString base = "http://gw:5001";
        QCOMPARE(ShareHelper::resolveThumbnail("ia:kuMUquaeE6g", base),
                 QStringLiteral("http://gw:5001/ipfs/ia:kuMUquaeE6g"));
        QCOMPARE(ShareHelper::resolveThumbnail("QmThumb123", base),
                 QStringLiteral("http://gw:5001/ipfs/QmThumb123"));
        QVERIFY(ShareHelper::resolveThumbnail("https://evil.example/x.png", base).isEmpty());
        QVERIFY(ShareHelper::resolveThumbnail("file:///etc/passwd", base).isEmpty());
        QVERIFY(ShareHelper::resolveThumbnail("../../x", base).isEmpty());
        QVERIFY(ShareHelper::resolveThumbnail("cid", QString()).isEmpty());   // no gateway → placeholder

        const QJsonArray cols{ col("c1", "Maps", 1, "mirrored", "https://evil.example/x") };
        const QJsonObject d = ShareHelper::buildShareData(cols, "me", base);
        QVERIFY(d.value("items").toArray()[0].toObject().value("thumbnail").toString().isEmpty());
    }

    void savePng_rejectsOversized()
    {
        QTemporaryDir dir;
        QString huge(17 * 1024 * 1024, QLatin1Char('A'));
        QCOMPARE(ShareHelper::savePngBase64(dir.path(), "x", huge).value("error").toString(),
                 QStringLiteral("png_too_large"));
    }

    void sanitize_stripsTraversal()
    {
        QCOMPARE(ShareHelper::sanitizeName("../../etc/passwd"), QStringLiteral("etcpasswd"));
        QCOMPARE(ShareHelper::sanitizeName("card 2026!.png"), QStringLiteral("card2026png"));
        QCOMPARE(ShareHelper::sanitizeName("ia-archive-card_1"), QStringLiteral("ia-archive-card_1"));
        QVERIFY(ShareHelper::sanitizeName("////....").isEmpty());
    }

    void savePng_writesValidPng()
    {
        QTemporaryDir dir;
        const QJsonObject r =
            ShareHelper::savePngBase64(dir.path() + "/cards", "my-card", validPngBase64());
        QVERIFY(r.value("ok").toBool());
        const QString path = r.value("path").toString();
        QVERIFY(path.endsWith("/cards/my-card.png"));
        QImage round(path);
        QVERIFY(!round.isNull());          // a real, loadable PNG landed on disk
        QCOMPARE(round.width(), 4);
    }

    void saveFromFile_movesValidPng()
    {
        QTemporaryDir dir;
        const QString tmp = dir.path() + "/grab.png";
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QVERIFY(img.save(tmp, "PNG"));
        const QJsonObject r = ShareHelper::saveFromFile(dir.path() + "/cards", "card-1", tmp);
        QVERIFY(r.value("ok").toBool());
        QVERIFY(QFile::exists(r.value("path").toString()));
        QVERIFY(!QFile::exists(tmp));   // tmp consumed
        QVERIFY(!QImage(r.value("path").toString()).isNull());
    }

    void saveFromFile_rejectsGarbageAndMissing()
    {
        QTemporaryDir dir;
        QCOMPARE(ShareHelper::saveFromFile(dir.path(), "x", dir.path() + "/nope.png")
                     .value("error").toString(), QStringLiteral("grab_file_missing"));
        const QString bad = dir.path() + "/bad.png";
        QFile f(bad); f.open(QIODevice::WriteOnly); f.write("not a png at all, padding padding padding padding padding padding"); f.close();
        QCOMPARE(ShareHelper::saveFromFile(dir.path(), "x", bad)
                     .value("error").toString(), QStringLiteral("invalid_png"));
    }

    void savePng_rejectsGarbage()
    {
        QTemporaryDir dir;
        QCOMPARE(ShareHelper::savePngBase64(dir.path(), "x", "!!!not-base64!!!")
                     .value("error").toString(), QStringLiteral("invalid_png"));
        QCOMPARE(ShareHelper::savePngBase64(dir.path(), "x",
                     QString::fromLatin1(QByteArray(4096, 'A').toBase64()))
                     .value("error").toString(), QStringLiteral("invalid_png"));
        QCOMPARE(ShareHelper::savePngBase64(dir.path(), "///", validPngBase64())
                     .value("error").toString(), QStringLiteral("invalid_name"));
    }
};

QTEST_MAIN(TstShareHelper)
#include "tst_share_helper.moc"
