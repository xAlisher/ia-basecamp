#include "mock_storage.h"

#include <memory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

MockStorage::MockStorage(QObject* parent) : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &MockStorage::handleConnection);
}

bool MockStorage::start()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QString MockStorage::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
}

void MockStorage::handleConnection()
{
    QTcpSocket* sock = m_server.nextPendingConnection();
    if (refuse) {
        sock->abort();
        sock->deleteLater();
        return;
    }
    auto buffer = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, this, [this, sock, buffer] {
        buffer->append(sock->readAll());
        if (!buffer->contains("\r\n\r\n"))
            return;
        const int lineEnd = buffer->indexOf("\r\n");
        const QList<QByteArray> parts = buffer->left(lineEnd).split(' ');
        if (parts.size() < 2)
            return;
        const QUrl url = QUrl::fromEncoded(parts[1]);
        const QUrlQuery q(url);
        const QString cid = q.queryItemValue(QStringLiteral("arg"));
        requestLog.append(url.path() + (url.hasQuery() ? "?" + url.query() : QString()));

        QByteArray body;
        int status = 200;
        const QString path = url.path();
        if (path == QLatin1String("/api/v0/version")) {
            body = R"({"Version":"0.0-mock"})";
        } else if (path == QLatin1String("/api/v0/pin/add")) {
            if (failPins) {
                status = 500;
                body = R"({"Message":"mock pin failure","Code":0})";
            } else {
                for (int i = 1; i <= progressSteps; ++i)
                    body += QJsonDocument(QJsonObject{ { "Progress", i * 10 } })
                                .toJson(QJsonDocument::Compact) + "\n";
                body += QJsonDocument(QJsonObject{ { "Pins", QJsonArray{ cid } } })
                            .toJson(QJsonDocument::Compact) + "\n";
                pinned.insert(cid);
                repoSize += 1000;
            }
        } else if (path == QLatin1String("/api/v0/pin/rm")) {
            if (pinned.remove(cid)) {
                repoSize -= 1000;
                body = QJsonDocument(QJsonObject{ { "Pins", QJsonArray{ cid } } })
                           .toJson(QJsonDocument::Compact);
            } else {
                status = 500;
                body = R"({"Message":"not pinned or pinned indirectly","Code":0})";
            }
        } else if (path == QLatin1String("/api/v0/pin/ls")) {
            if (pinned.contains(cid)) {
                body = QJsonDocument(QJsonObject{
                    { "Keys", QJsonObject{ { cid, QJsonObject{ { "Type", "recursive" } } } } } })
                           .toJson(QJsonDocument::Compact);
            } else {
                status = 500;
                body = R"({"Message":"path is not pinned","Code":0})";
            }
        } else if (path == QLatin1String("/api/v0/repo/stat")) {
            body = QJsonDocument(QJsonObject{ { "RepoSize", repoSize }, { "NumObjects", 1 } })
                       .toJson(QJsonDocument::Compact);
        } else {
            status = 404;
            body = R"({"error":"not found"})";
        }

        const QByteArray head = "HTTP/1.1 " + QByteArray::number(status)
            + (status == 200 ? " OK" : " Error")
            + "\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
        if (fragmentStream && path == QLatin1String("/api/v0/pin/add") && body.size() > 8) {
            // split mid-JSON-line to exercise the client's readyRead/finished boundary
            const int cut = body.size() / 2;
            sock->write(head + body.left(cut));
            sock->flush();
            const QByteArray rest = body.mid(cut);
            QTimer::singleShot(50, sock, [sock, rest] {
                sock->write(rest);
                sock->disconnectFromHost();
            });
        } else {
            sock->write(head + body);
            sock->disconnectFromHost();
        }
    });
    connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
}
