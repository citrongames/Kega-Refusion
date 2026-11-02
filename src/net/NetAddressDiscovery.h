#pragma once

#include <QObject>
#include "NetInterfaces.h"
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVector>

class BoundHttpRequest;

// One-shot, asynchronous public IPv4 discovery after the user creates a lobby.
// UPnP reads router status only; it never creates or changes port mappings.
class NetAddressDiscovery final : public QObject {
    Q_OBJECT
public:
    explicit NetAddressDiscovery(QObject* parent = nullptr);
    QString connectionName() const { return connection_.name; }
    static bool isPublicIpv4(const QString& address);

public slots:
    void start(const QString& connectionId = {});
    void cancel();

signals:
    // Empty address means no usable global IPv4 was found.
    void finished(const QString& publicIpv4, const QString& detail);

private:
    struct Router { QUrl description; QString responder; };
    void readSsdp();
    void queryRouters();
    void fetchDescription(const Router& router);
    void queryExternalAddress(const QUrl& control, const QString& serviceType);
    void fallbackHttps(int serviceIndex = 0);
    void finish(const QString& address, const QString& detail);
    bool active_ = false;
    bool attempted_ = false;
    int generation_ = 0;
    int routerReplies_ = 0;
    QVector<Router> routers_;
    QVector<QPointer<QNetworkReply>> replies_;
    QUdpSocket socket_;
    QTimer discoveryTimer_, upnpTimer_;
    QNetworkAccessManager routerNetwork_;
    NetInterfaces::Connection connection_;
    QPointer<BoundHttpRequest> publicRequest_;
    bool usingHttps_ = false;
};
