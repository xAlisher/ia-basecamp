#include "share_helper.h"

#include <QDir>
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

QJsonObject itemFor(const QJsonObject& c)
{
    return {
        { QStringLiteral("name"), c.value(QLatin1String("title")).toString() },
        { QStringLiteral("sizeGB"), toGb(c.value(QLatin1String("sizeBytes")).toVariant().toLongLong()) },
        { QStringLiteral("thumbnail"), c.value(QLatin1String("thumbnail")).toString() },
    };
}

} // namespace

QJsonObject buildShareData(const QJsonArray& collections, const QString& scope)
{
    QJsonArray items;
    qint64 totalBytes = 0;

    if (scope == QLatin1String("me")) {
        for (const QJsonValue& v : collections) {
            const QJsonObject c = v.toObject();
            if (c.value(QLatin1String("state")).toString() != QLatin1String("mirrored"))
                continue;
            items.append(itemFor(c));
            totalBytes += c.value(QLatin1String("sizeBytes")).toVariant().toLongLong();
        }
        if (items.isEmpty())
            return fail(QStringLiteral("nothing_preserved_yet"));
        return {
            { QStringLiteral("ok"), true },
            { QStringLiteral("scope"), QStringLiteral("me") },
            { QStringLiteral("title"), QStringLiteral("I'm preserving the archive") },
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
            { QStringLiteral("items"), QJsonArray{ itemFor(c) } },
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

QJsonObject savePngBase64(const QString& dir, const QString& name, const QString& pngBase64)
{
    const QString safeName = sanitizeName(name);
    if (safeName.isEmpty())
        return fail(QStringLiteral("invalid_name"));

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
