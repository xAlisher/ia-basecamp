#ifndef LEZ_CLIENT_H
#define LEZ_CLIENT_H
#include <QObject>
class QNetworkAccessManager;
// P1: LEZ indexer JSON-RPC reads — getTransactionsByAccount / getAccount / getTransaction /
// getLastFinalizedBlockId, decode Op::ChannelInscribe(InscriptionOp) into collection records.
// Federated gateway list + failover. Finalized data only (PoS reorgs above LIB). See SPEC.md §4.
class LezClient : public QObject {
    Q_OBJECT
public:
    explicit LezClient(QObject* parent = nullptr);
private:
    QNetworkAccessManager* m_net = nullptr;
};
#endif
