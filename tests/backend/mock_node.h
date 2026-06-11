#ifndef MOCK_NODE_H
#define MOCK_NODE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>

// Minimal deterministic HTTP/1.1 stand-in for a gateway node's Cryptarchia API.
// Serves /cryptarchia/info from `info` and /cryptarchia/blocks?slot_from&slot_to by
// slicing `blocks` on header.slot. No network, no threads, no real node.
class MockNode : public QObject {
    Q_OBJECT
public:
    explicit MockNode(QObject* parent = nullptr);

    bool start();                       // listen on 127.0.0.1, random port
    QString baseUrl() const;            // http://127.0.0.1:<port>

    QJsonObject info;                   // returned verbatim from /cryptarchia/info
    QJsonArray blocks;                  // full chain; sliced per request
    QJsonObject channelInfo;            // /api/channel/* (explorer stand-in); empty = 404
    bool refuse = false;                // simulate a dead gateway (close immediately)
    int failAfterRequests = -1;         // die mid-conversation after N served requests
    int requestCount = 0;

private:
    void handleConnection();
    QTcpServer m_server;
};

#endif // MOCK_NODE_H
