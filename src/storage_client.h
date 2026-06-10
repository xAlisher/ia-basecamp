#ifndef STORAGE_CLIENT_H
#define STORAGE_CLIENT_H
#include <QObject>
class QNetworkAccessManager;
// P2: Storage HTTP client — pin/replicate a collection's CIDs. mode delegate (trusted node pins) |
// local (your node replicates to disk). Streams progress. See SPEC.md §5.
class StorageClient : public QObject {
    Q_OBJECT
public:
    explicit StorageClient(QObject* parent = nullptr);
private:
    QNetworkAccessManager* m_net = nullptr;
};
#endif
