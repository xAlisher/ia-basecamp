#include "ia_files.h"

#include <QCryptographicHash>
#include <QFile>
#include <QXmlStreamReader>

QList<IaFileEntry> parseIaFilesXml(const QByteArray& xml)
{
    QList<IaFileEntry> out;
    QXmlStreamReader r(xml);
    IaFileEntry cur;
    bool inFile = false;
    while (!r.atEnd()) {
        switch (r.readNext()) {
        case QXmlStreamReader::StartElement:
            if (r.name() == QLatin1String("file")) {
                cur = IaFileEntry{};
                cur.name = r.attributes().value(QLatin1String("name")).toString();
                inFile = true;
            } else if (inFile && r.name() == QLatin1String("md5")) {
                cur.md5 = r.readElementText().trimmed().toLower();
            } else if (inFile && r.name() == QLatin1String("sha1")) {
                cur.sha1 = r.readElementText().trimmed().toLower();
            } else if (inFile && r.name() == QLatin1String("size")) {
                cur.size = r.readElementText().trimmed().toLongLong();
            }
            break;
        case QXmlStreamReader::EndElement:
            if (r.name() == QLatin1String("file") && inFile) {
                if (!cur.name.isEmpty())
                    out.append(cur);
                inFile = false;
            }
            break;
        default:
            break;
        }
    }
    if (r.hasError())
        return {};   // a manifest we cannot parse verifies nothing — caller fails the item
    return out;
}

IaVerify verifyIaFile(const QString& path, const IaFileEntry& entry)
{
    const bool useMd5 = !entry.md5.isEmpty();
    if (!useMd5 && entry.sha1.isEmpty())
        return IaVerify::Unverified;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return IaVerify::Mismatch;   // unreadable bytes are unverifiable bytes

    QCryptographicHash hash(useMd5 ? QCryptographicHash::Md5 : QCryptographicHash::Sha1);
    if (!hash.addData(&f))
        return IaVerify::Mismatch;
    const QString got = QString::fromLatin1(hash.result().toHex());
    const QString want = useMd5 ? entry.md5 : entry.sha1;
    return got == want ? IaVerify::Verified : IaVerify::Mismatch;
}
