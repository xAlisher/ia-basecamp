#include "storage_client.h"
#include <QNetworkAccessManager>
StorageClient::StorageClient(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}
