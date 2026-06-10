#include "share_helper.h"

#include <QDir>
#include <QFile>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>

namespace ShareHelper {

namespace {

QJsonObject fail(const QString& code)
{
    return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), code } };
}

double toGb(qint64 bytes)
{
    return static_cast<double>(bytes) / 1e9;
}

QJsonObject itemFor(const QJsonObject& c, const QString& thumbnailBase)
{
    return {
        { QStringLiteral("name"), c.value(QLatin1String("title")).toString() },
        { QStringLiteral("sizeGB"), toGb(c.value(QLatin1String("sizeBytes")).toVariant().toLongLong()) },
        { QStringLiteral("thumbnail"),
          resolveThumbnail(c.value(QLatin1String("thumbnail")).toString(), thumbnailBase) },
    };
}

} // namespace

QString resolveThumbnail(const QString& thumbnail, const QString& base)
{
    if (thumbnail.isEmpty() || base.isEmpty())
        return {};
    static const QRegularExpression cidShape(QStringLiteral("^[A-Za-z0-9:._-]{1,128}$"));
    if (!cidShape.match(thumbnail).hasMatch())
        return {};   // a URL or anything exotic from the chain never reaches Image.source
    return base + QStringLiteral("/ipfs/") + thumbnail;
}

QJsonObject buildShareData(const QJsonArray& collections, const QString& scope,
                           const QString& thumbnailBase, qint64 fallbackUsedBytes)
{
    QJsonArray items;
    qint64 totalBytes = 0;

    if (scope == QLatin1String("me")) {
        for (const QJsonValue& v : collections) {
            const QJsonObject c = v.toObject();
            if (c.value(QLatin1String("state")).toString() != QLatin1String("mirrored"))
                continue;
            items.append(itemFor(c, thumbnailBase));
            totalBytes += c.value(QLatin1String("sizeBytes")).toVariant().toLongLong();
        }
        if (items.isEmpty())
            return fail(QStringLiteral("nothing_preserved_yet"));
        if (totalBytes == 0)
            totalBytes = fallbackUsedBytes;   // manifests without sizes → real stored bytes
        return {
            { QStringLiteral("ok"), true },
            { QStringLiteral("scope"), QStringLiteral("me") },
            { QStringLiteral("title"), QStringLiteral("I'm preserving the archive") },
            { QStringLiteral("totalBytes"), totalBytes },
            { QStringLiteral("totalGB"), toGb(totalBytes) },
            { QStringLiteral("count"), items.size() },
            { QStringLiteral("items"), items },
        };
    }

    for (const QJsonValue& v : collections) {
        const QJsonObject c = v.toObject();
        if (c.value(QLatin1String("id")).toString() != scope)
            continue;
        return {
            { QStringLiteral("ok"), true },
            { QStringLiteral("scope"), scope },
            { QStringLiteral("title"), c.value(QLatin1String("title")).toString() },
            { QStringLiteral("totalGB"), toGb(c.value(QLatin1String("sizeBytes")).toVariant().toLongLong()) },
            { QStringLiteral("count"), 1 },
            { QStringLiteral("txHash"), c.value(QLatin1String("txHash")).toString() },
            { QStringLiteral("items"), QJsonArray{ itemFor(c, thumbnailBase) } },
        };
    }
    return fail(QStringLiteral("unknown_collection"));
}

QString sanitizeName(const QString& name)
{
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9_-]"));
    QString s = name;
    s.remove(bad);
    return s.left(64);
}

QJsonObject saveFromFile(const QString& dir, const QString& name, const QString& tmpPath)
{
    const QString safeName = sanitizeName(name);
    if (safeName.isEmpty())
        return fail(QStringLiteral("invalid_name"));
    QFile in(tmpPath);
    if (!in.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("grab_file_missing"));
    if (in.size() > 16 * 1024 * 1024)
        return fail(QStringLiteral("png_too_large"));
    const QByteArray png = in.readAll();
    in.close();
    static const QByteArray pngMagic = QByteArray::fromHex("89504e470d0a1a0a");
    if (png.size() < 67 || !png.startsWith(pngMagic))
        return fail(QStringLiteral("invalid_png"));
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("cannot_create_dir"));
    const QString path = dir + QLatin1Char('/') + safeName + QStringLiteral(".png");
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("cannot_write"));
    f.write(png);
    if (!f.commit())
        return fail(QStringLiteral("cannot_write"));
    QFile::remove(tmpPath);
    return { { QStringLiteral("ok"), true }, { QStringLiteral("path"), path } };
}

QJsonObject savePngBase64(const QString& dir, const QString& name, const QString& pngBase64)
{
    const QString safeName = sanitizeName(name);
    if (safeName.isEmpty())
        return fail(QStringLiteral("invalid_name"));

    // a 1200×675 card is ~1–3 MB; anything past this cap is hostile, refuse the allocation
    constexpr qsizetype kMaxBase64Chars = 16 * 1024 * 1024;
    if (pngBase64.size() > kMaxBase64Chars)
        return fail(QStringLiteral("png_too_large"));

    const QByteArray png =
        QByteArray::fromBase64(pngBase64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    static const QByteArray pngMagic = QByteArray::fromHex("89504e470d0a1a0a");
    if (png.size() < 67 || !png.startsWith(pngMagic))   // 67 = the minimal valid PNG
        return fail(QStringLiteral("invalid_png"));

    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("cannot_create_dir"));
    const QString path = dir + QLatin1Char('/') + safeName + QStringLiteral(".png");
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("cannot_write"));
    f.write(png);
    if (!f.commit())
        return fail(QStringLiteral("cannot_write"));
    return { { QStringLiteral("ok"), true }, { QStringLiteral("path"), path } };
}

} // namespace ShareHelper
