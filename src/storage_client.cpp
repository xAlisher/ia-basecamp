#include "storage_client.h"

#include <memory>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

StorageClient::StorageClient(QObject* parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
}

void StorageClient::setEndpoint(const QString& baseUrl)
{
    m_endpoint = baseUrl;
}

QString StorageClient::resolveEndpoint(const QString& mode, const QString& gatewayStorageUrl,
                                       const QString& localUrl)
{
    if (mode == QLatin1String("local"))
        return localUrl;
    return gatewayStorageUrl;   // delegate (default): the trusted gateway pins
}

// Abort before buffering an oversized body — a post-read .left() cap would already
// have paid the allocation. Pin's streaming consumer drains continuously, so its
// buffered watermark stays small and this guard never fires on legitimate streams.
void StorageClient::guardBodySize(QNetworkReply* reply)
{
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > kMaxBodyBytes)
            reply->abort();
    });
}

QNetworkReply* StorageClient::httpPost(const QString& path, const QString& query)
{
    QUrl url(m_endpoint);
    QString base = url.path();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    url.setPath(base + path);
    if (!query.isEmpty())
        url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(kHttpTimeoutMs);
    // Kubo's CSRF protection 403s browser-like callers (Mozilla/* UA, as Qt sends by
    // default) — identify as a CLI-class client
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ia-basecamp/0.1"));
    return m_net->post(req, QByteArray());   // Kubo RPC is POST-only, empty body
}

void StorageClient::pollHealth()
{
    QNetworkReply* reply = httpPost(QStringLiteral("/api/v0/version"));
    guardBodySize(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const bool up = reply->error() == QNetworkReply::NoError;
        m_storageState = up ? QStringLiteral("ready") : QStringLiteral("offline");
        emit healthChanged(m_storageState);
    });
}

void StorageClient::pin(const QString& cid)
{
    if (m_pinning.contains(cid)) {
        emit pinFinished(cid, false, QStringLiteral("pin_in_progress"));
        return;
    }
    m_pinning.insert(cid);

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("arg"), cid);
    q.addQueryItem(QStringLiteral("progress"), QStringLiteral("true"));
    QNetworkReply* reply = httpPost(QStringLiteral("/api/v0/pin/add"), q.toString());

    // pin/add streams JSON lines: {"Progress":n}... then {"Pins":["<cid>"]} on success.
    // Lines arrive across readyRead AND the final buffer — track the outcome across both.
    auto pinnedSeen = std::make_shared<bool>(false);
    auto errMsg = std::make_shared<QString>();
    const auto consumeLine = [this, cid, pinnedSeen, errMsg](const QByteArray& raw) {
        if (raw.trimmed().isEmpty())
            return;
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        if (o.contains(QLatin1String("Progress")))
            emit pinProgress(cid, o.value(QLatin1String("Progress")).toVariant().toLongLong());
        if (o.contains(QLatin1String("Pins")))
            *pinnedSeen = true;
        if (o.contains(QLatin1String("Message")))
            *errMsg = o.value(QLatin1String("Message")).toString();
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, consumeLine] {
        while (reply->canReadLine())
            consumeLine(reply->readLine());
    });
    // after draining whole lines, anything still buffered is one partial line —
    // a line that alone exceeds the cap is hostile
    guardBodySize(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cid, pinnedSeen, errMsg, consumeLine] {
        reply->deleteLater();
        m_pinning.remove(cid);
        if (reply->error() != QNetworkReply::NoError && !reply->bytesAvailable() && errMsg->isEmpty()) {
            emit pinFinished(cid, false, QStringLiteral("storage_unreachable"));
            return;
        }
        const QList<QByteArray> lines = reply->readAll().left(kMaxBodyBytes).split('\n');
        for (const QByteArray& raw : lines)
            consumeLine(raw);
        if (*pinnedSeen)
            emit pinFinished(cid, true, QString());
        else
            emit pinFinished(cid, false,
                             errMsg->isEmpty() ? QStringLiteral("pin_failed") : *errMsg);
    });
}

void StorageClient::unpin(const QString& cid)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("arg"), cid);
    QNetworkReply* reply = httpPost(QStringLiteral("/api/v0/pin/rm"), q.toString());
    guardBodySize(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cid] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() == QNetworkReply::NoError) {
            emit unpinFinished(cid, true, QString());
            return;
        }
        // "not pinned" is success for unmirror purposes — the end state is identical
        if (body.contains("not pinned")) {
            emit unpinFinished(cid, true, QString());
            return;
        }
        // server-side failures keep Kubo's message (same fidelity as pin)
        const QString msg =
            QJsonDocument::fromJson(body).object().value(QLatin1String("Message")).toString();
        emit unpinFinished(cid, false,
                           msg.isEmpty() ? QStringLiteral("storage_unreachable") : msg);
    });
}

void StorageClient::queryPinned(const QString& cid)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("arg"), cid);
    QNetworkReply* reply = httpPost(QStringLiteral("/api/v0/pin/ls"), q.toString());
    guardBodySize(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cid] {
        reply->deleteLater();
        const QByteArray body = reply->readAll().left(kMaxBodyBytes);
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        const bool pinned = reply->error() == QNetworkReply::NoError
                            && o.value(QLatin1String("Keys")).toObject().contains(cid);
        emit pinnedResult(cid, pinned);
    });
}

void StorageClient::queryRepoStat()
{
    QNetworkReply* reply = httpPost(QStringLiteral("/api/v0/repo/stat"));
    guardBodySize(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonObject o =
            QJsonDocument::fromJson(reply->readAll().left(kMaxBodyBytes)).object();
        if (o.contains(QLatin1String("RepoSize")))
            emit repoStatResult(o.value(QLatin1String("RepoSize")).toVariant().toLongLong());
    });
}
