#include "mock_node.h"

#include <memory>

#include <QJsonDocument>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

MockNode::MockNode(QObject* parent) : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &MockNode::handleConnection);
}

bool MockNode::start()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QString MockNode::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
}

void MockNode::handleConnection()
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
            return;   // wait for the full (fragmented) request head
        const QByteArray req = *buffer;
        const int lineEnd = req.indexOf("\r\n");
        const QList<QByteArray> parts = req.left(lineEnd).split(' ');
        if (parts.size() < 2)
            return;
        ++requestCount;
        const QUrl url = QUrl::fromEncoded(parts[1]);

        QByteArray body = "{}";
        int status = 200;
        if (url.path() == QLatin1String("/cryptarchia/info")) {
            body = QJsonDocument(info).toJson(QJsonDocument::Compact);
        } else if (url.path() == QLatin1String("/cryptarchia/blocks")) {
            const QUrlQuery q(url);
            const qint64 from = q.queryItemValue(QStringLiteral("slot_from")).toLongLong();
            const qint64 to = q.queryItemValue(QStringLiteral("slot_to")).toLongLong();
            QJsonArray slice;
            for (const QJsonValue& bv : std::as_const(blocks)) {
                const qint64 slot = bv.toObject()
                                        .value(QLatin1String("header")).toObject()
                                        .value(QLatin1String("slot")).toVariant().toLongLong();
                if (slot >= from && slot <= to)
                    slice.append(bv);
            }
            body = QJsonDocument(slice).toJson(QJsonDocument::Compact);
        } else {
            status = 404;
            body = R"({"error":"not found"})";
        }

        const QByteArray resp = "HTTP/1.1 " + QByteArray::number(status)
            + (status == 200 ? " OK" : " Not Found")
            + "\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        sock->write(resp);
        sock->disconnectFromHost();
    });
    connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
}
