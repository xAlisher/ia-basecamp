#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

#include "ia_files.h"

// Campaign preserve (#14): the parser and verify policy are the trust boundary
// between archive.org bytes and Logos Storage — pinned deterministically here.

class TstIaFiles : public QObject {
    Q_OBJECT

private slots:
    void parse_realShape()
    {
        // the exact shape IA serves for {id}_files.xml (abridged)
        const QByteArray xml = R"(<files>
  <file name="__ia_thumb.jpg" source="original">
    <mtime>1781100000</mtime>
    <size>13138</size>
    <md5>0123456789abcdef0123456789abcdef</md5>
    <sha1>da39a3ee5e6b4b0d3255bfef95601890afd80709</sha1>
  </file>
  <file name="photo-metro-august-1991_files.xml" source="metadata">
    <size>7817</size>
  </file>
  <file name="meta.sqlite" source="metadata">
    <md5>FEDCBA9876543210fedcba9876543210</md5>
  </file>
</files>)";
        const auto files = parseIaFilesXml(xml);
        QCOMPARE(files.size(), 3);
        QCOMPARE(files[0].name, QStringLiteral("__ia_thumb.jpg"));
        QCOMPARE(files[0].md5, QStringLiteral("0123456789abcdef0123456789abcdef"));
        QCOMPARE(files[0].sha1, QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
        QCOMPARE(files[0].size, qint64(13138));
        // the manifest can't checksum itself — entry survives, hashes empty
        QCOMPARE(files[1].name, QStringLiteral("photo-metro-august-1991_files.xml"));
        QVERIFY(files[1].md5.isEmpty());
        // checksums normalize to lowercase
        QCOMPARE(files[2].md5, QStringLiteral("fedcba9876543210fedcba9876543210"));
    }

    void parse_garbageYieldsNothing()
    {
        QVERIFY(parseIaFilesXml("not xml at all").isEmpty());
        QVERIFY(parseIaFilesXml("<files><file name=\"x\"").isEmpty());   // truncated
        QVERIFY(parseIaFilesXml("<files></files>").isEmpty());
    }

    void verify_md5Match()
    {
        QTemporaryDir tmp;
        const QString path = tmp.path() + "/f.bin";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("logos-ia campaign bytes");
        f.close();

        IaFileEntry e;
        e.md5 = QString::fromLatin1(QCryptographicHash::hash(
            "logos-ia campaign bytes", QCryptographicHash::Md5).toHex());
        QCOMPARE(int(verifyIaFile(path, e)), int(IaVerify::Verified));

        e.md5 = QStringLiteral("00000000000000000000000000000000");
        QCOMPARE(int(verifyIaFile(path, e)), int(IaVerify::Mismatch));
    }

    void verify_sha1FallbackWhenNoMd5()
    {
        QTemporaryDir tmp;
        const QString path = tmp.path() + "/f.bin";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("verified via sha1");
        f.close();

        IaFileEntry e;
        e.sha1 = QString::fromLatin1(QCryptographicHash::hash(
            "verified via sha1", QCryptographicHash::Sha1).toHex());
        QCOMPARE(int(verifyIaFile(path, e)), int(IaVerify::Verified));
    }

    void verify_noChecksumIsUnverifiedNotPass()
    {
        QTemporaryDir tmp;
        const QString path = tmp.path() + "/f.bin";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QCOMPARE(int(verifyIaFile(path, IaFileEntry{})), int(IaVerify::Unverified));
    }

    void verify_unreadableIsMismatch()
    {
        IaFileEntry e;
        e.md5 = QStringLiteral("00000000000000000000000000000000");
        QCOMPARE(int(verifyIaFile("/nonexistent/path", e)), int(IaVerify::Mismatch));
    }
};

QTEST_GUILESS_MAIN(TstIaFiles)
#include "tst_ia_files.moc"
