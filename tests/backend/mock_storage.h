#ifndef MOCK_STORAGE_H
#define MOCK_STORAGE_H

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTcpServer>

// Deterministic stand-in for a Kubo-compatible Storage RPC endpoint.
// Tracks pinned CIDs; pin/add streams progress lines before the final Pins line.
class MockStorage : public QObject {
    Q_OBJECT
public:
    explicit MockStorage(QObject* parent = nullptr);

    bool start();
    QString baseUrl() const;

    QSet<QString> pinned;
    qint64 repoSize = 0;
    int progressSteps = 2;       // {"Progress":n} lines emitted before success
    bool failPins = false;       // pin/add returns a Kubo-style error message
    bool refuse = false;         // dead endpoint
    bool fragmentStream = false; // write pin/add body in delayed chunks (split mid-line)
    QStringList requestLog;      // "<path>?<query>" per request, in order

private:
    void handleConnection();
    QTcpServer m_server;
};

#endif // MOCK_STORAGE_H
