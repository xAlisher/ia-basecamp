#include "lez_client.h"
#include <QNetworkAccessManager>
LezClient::LezClient(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}
