#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVector>

// One-shot, asynchronous public IPv4 discovery after the user creates a lobby.
// UPnP reads router status only; it never creates or changes port mappings.
class NetAddressDiscovery final : public QObject {
    Q_OBJECT
public:
    explicit NetAddressDiscovery(QObject* parent = nullptr);
    static bool isPublicIpv4(const QString& address);

public slots:
    void start();
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
    QNetworkAccessManager network_;
    bool usingHttps_ = false;
};
