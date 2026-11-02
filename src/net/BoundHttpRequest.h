#pragma once

#include "net/NetInterfaces.h"

#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QSslSocket;

// One HTTPS public-IP lookup pinned to a selected local IPv4 adapter.
class BoundHttpRequest final : public QObject {
    Q_OBJECT
public:
    explicit BoundHttpRequest(QObject* parent = nullptr);
    ~BoundHttpRequest() override;
    void start(const QUrl& url, const NetInterfaces::Connection& connection, int timeoutMs = 2500);
    void cancel();

signals:
    void finished(const QByteArray& body, const QString& error);

private:
    void readAvailable();
    void complete(const QByteArray& body, const QString& error);
    bool parseHeaders(const QByteArray& header, QString* error);

    QSslSocket* socket_ = nullptr;
    qintptr boundDescriptor_ = -1;
    QTimer timer_;
    QByteArray header_;
    QByteArray body_;
    qint64 contentLength_ = -1;
    bool headersDone_ = false;
    bool started_ = false;
    bool completed_ = false;
};
