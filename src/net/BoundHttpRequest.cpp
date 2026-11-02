#include "net/BoundHttpRequest.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QSslSocket>

namespace {
constexpr int HeaderLimit = 8 * 1024;
constexpr int BodyLimit = 64;
constexpr int ReadLimit = HeaderLimit + BodyLimit + 512;

bool decimal(const QByteArray& value) {
    if (value.isEmpty()) return false;
    for (const char ch : value) if (ch < 48 || ch > 57) return false;
    return true;
}
}

BoundHttpRequest::BoundHttpRequest(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        complete({}, QStringLiteral("Address lookup timed out"));
    });
}

BoundHttpRequest::~BoundHttpRequest() {
    timer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

void BoundHttpRequest::start(const QUrl& url, const NetInterfaces::Connection& choice, int timeoutMs) {
    if (started_) return;
    started_ = true;
    const QHostAddress local(choice.ipv4);
    const bool badUrl = !url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host().isEmpty() || url.hasFragment() ||
        url.authority(QUrl::FullyEncoded).contains(QLatin1Char(64)) ||
        url.toEncoded().size() > 2048 || url.port(443) < 1 || url.port(443) > 65535;
    if (badUrl || choice.id.isEmpty() || choice.index == 0 ||
        local.protocol() != QAbstractSocket::IPv4Protocol ||
        timeoutMs < 1 || timeoutMs > 30000) {
        complete({}, QStringLiteral("Invalid HTTPS lookup or selected interface"));
        return;
    }
    QByteArray path = url.path(QUrl::FullyEncoded).toUtf8();
    if (path.isEmpty()) path = QByteArrayLiteral("/");
    if (!url.query().isEmpty()) path += QByteArrayLiteral("?") + url.query(QUrl::FullyEncoded).toUtf8();
    const QByteArray host = QUrl::toAce(url.host());
    if (host.isEmpty() || path.size() > 2048) {
        complete({}, QStringLiteral("Invalid HTTPS hostname or path"));
        return;
    }
    QByteArray request = QByteArrayLiteral("GET ") + path + QByteArrayLiteral(" HTTP/1.0\r\nHost: ") + host;
    if (url.port(443) != 443) request += QByteArrayLiteral(":") + QByteArray::number(url.port());
    request += QByteArrayLiteral("\r\nAccept: text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");

    socket_ = new QSslSocket(this);
    socket_->setProxy(QNetworkProxy::NoProxy);
    socket_->setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket_->setPeerVerifyName(url.host());
    socket_->setReadBufferSize(ReadLimit);
    connect(socket_, &QSslSocket::connected, this, [this, local] {
        if (socket_->socketDescriptor() != boundDescriptor_ || socket_->localAddress() != local)
            complete({}, QStringLiteral("Selected interface changed during connect"));
    });
    connect(socket_, &QSslSocket::encrypted, this, [this, request, local] {
        if (socket_->socketDescriptor() != boundDescriptor_ ||
            socket_->localAddress() != local || socket_->write(request) != request.size())
            complete({}, QStringLiteral("Selected interface or HTTPS request failed"));
    });
    connect(socket_, &QSslSocket::readyRead, this, &BoundHttpRequest::readAvailable);
    connect(socket_, &QSslSocket::disconnected, this, [this] {
        readAvailable();
        if (completed_) return;
        if (socket_ && socket_->bytesAvailable() > 0) {
            complete({}, QStringLiteral("HTTPS response exceeds limits"));
            return;
        }
        if (!headersDone_ || (contentLength_ >= 0 && body_.size() != contentLength_))
            complete({}, QStringLiteral("Incomplete HTTPS response"));
        else complete(body_, {});
    });
    connect(socket_, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        // HTTP/1.0 without Content-Length normally completes at EOF. Qt reports
        // RemoteHostClosedError before disconnected, where framing is finalized.
        if (error != QAbstractSocket::RemoteHostClosedError && !completed_)
            complete({}, socket_->errorString());
    });
    connect(socket_, &QSslSocket::sslErrors, this, [this] {
        complete({}, QStringLiteral("HTTPS certificate verification failed"));
    });
    if (!socket_->bind(local, 0)) {
        complete({}, QStringLiteral("Cannot bind selected local IPv4"));
        return;
    }
    QString bindError;
    if (socket_->socketDescriptor() < 0 ||
        !NetInterfaces::bindSocket(socket_->socketDescriptor(), choice, &bindError)) {
        complete({}, bindError.isEmpty() ? QStringLiteral("Cannot bind selected interface") : bindError);
        return;
    }
    boundDescriptor_ = socket_->socketDescriptor();
    timer_.start(timeoutMs); // Covers asynchronous DNS, TCP, TLS and response body.
    socket_->connectToHostEncrypted(url.host(), quint16(url.port(443)),
                                    QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
}

void BoundHttpRequest::cancel() {
    if (started_ && !completed_) complete({}, QStringLiteral("Address lookup cancelled"));
}

bool BoundHttpRequest::parseHeaders(const QByteArray& header, QString* error) {
    const QList<QByteArray> lines = header.split(10);
    if (lines.isEmpty()) { *error = QStringLiteral("Empty HTTPS response"); return false; }
    QByteArray status = lines.first().trimmed();
    if ((!status.startsWith("HTTP/1.0 ") && !status.startsWith("HTTP/1.1 ")) ||
        status.mid(9, 3) != QByteArrayLiteral("200") ||
        (status.size() > 12 && status.at(12) != 32)) {
        *error = QStringLiteral("HTTPS lookup returned a non-200 response");
        return false;
    }
    bool lengthSeen = false;
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith(13)) line.chop(1);
        if (line.isEmpty()) continue;
        if (line.startsWith(32) || line.startsWith(9)) {
            *error = QStringLiteral("Folded HTTP header rejected"); return false;
        }
        const int colon = line.indexOf(58);
        if (colon < 1) { *error = QStringLiteral("Invalid HTTP header"); return false; }
        const QByteArray key = line.left(colon).toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key == "transfer-encoding" || key == "content-encoding") {
            *error = QStringLiteral("Encoded HTTPS response rejected"); return false;
        }
        if (key == "content-length") {
            bool ok = false;
            const qint64 length = value.toLongLong(&ok);
            if (lengthSeen || !decimal(value) || !ok || length > BodyLimit) {
                *error = QStringLiteral("Invalid HTTPS response length"); return false;
            }
            lengthSeen = true;
            contentLength_ = length;
        }
    }
    return true;
}

void BoundHttpRequest::readAvailable() {
    if (completed_ || !socket_) return;
    const QByteArray incoming = socket_->read(ReadLimit);
    if (incoming.isEmpty()) return;
    if (!headersDone_) {
        header_ += incoming;
        const qsizetype separator = header_.indexOf("\r\n\r\n");
        if (separator < 0) {
            if (header_.size() > HeaderLimit) complete({}, QStringLiteral("HTTPS headers too large"));
        } else {
            if (separator + 4 > HeaderLimit) {
                complete({}, QStringLiteral("HTTPS headers too large")); return;
            }
            QString error;
            if (!parseHeaders(header_.left(separator), &error)) { complete({}, error); return; }
            headersDone_ = true;
            body_ = header_.mid(separator + 4);
            header_.clear();
        }
    } else body_ += incoming;
    if (completed_) return;
    if (body_.size() > BodyLimit || (contentLength_ >= 0 && body_.size() > contentLength_)) {
        complete({}, QStringLiteral("HTTPS response body too large")); return;
    }
    if (headersDone_ && contentLength_ >= 0 && body_.size() == contentLength_) {
        complete(body_, {}); return;
    }
    if (socket_->bytesAvailable() > 0)
        QTimer::singleShot(0, this, &BoundHttpRequest::readAvailable);
}

void BoundHttpRequest::complete(const QByteArray& body, const QString& error) {
    if (completed_) return;
    completed_ = true;
    timer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    emit finished(body, error);
}
