#include "NetAddressDiscovery.h"
#include "BoundHttpRequest.h"
#include <QNetworkInterface>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <memory>

namespace {
constexpr int MaxRouters = 2; // Description + SOAP: at most four router HTTP requests.
constexpr int DescriptionLimit = 128 * 1024;
constexpr int SoapLimit = 8 * 1024;
struct Body { QByteArray bytes; bool overflow = false; };

bool parseIpv4(const QString& text, quint32* value) {
    static const QRegularExpression dotted(
        QStringLiteral(R"(^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$)"));
    const auto match = dotted.match(text);
    if (!match.hasMatch()) return false;
    quint32 address = 0;
    for (int i = 1; i <= 4; ++i) {
        const auto octet = match.captured(i).toUInt();
        if (octet > 255) return false;
        address = (address << 8) | octet;
    }
    *value = address;
    return true;
}
bool privateRouter(const QString& text) {
    quint32 address = 0;
    if (!parseIpv4(text, &address)) return false;
    return (address & 0xff000000u) == 0x0a000000u ||
           (address & 0xfff00000u) == 0xac100000u ||
           (address & 0xffff0000u) == 0xc0a80000u;
}
bool pinnedRouterUrl(const QUrl& url, const QString& responder, int port = -1) {
    if (!url.isValid() || url.scheme() != QStringLiteral("http") ||
        !url.userName().isEmpty() || !url.password().isEmpty() ||
        !url.fragment().isEmpty() || url.toString().size() > 2048 ||
        !privateRouter(responder) || url.host() != responder ||
        url.port(80) < 1 || url.port(80) > 65535) return false;
    return port < 0 || url.port(80) == port;
}
bool httpOk(QNetworkReply* reply) {
    return reply->error() == QNetworkReply::NoError &&
           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
           !reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid();
}
std::shared_ptr<Body> collect(QNetworkReply* reply, QObject* context, int limit) {
    auto body = std::make_shared<Body>();
    reply->setReadBufferSize(limit + 1);
    QObject::connect(reply, &QIODevice::readyRead, context, [reply,body,limit] {
        if (body->overflow) return;
        if (reply->bytesAvailable() > limit - body->bytes.size()) {
            body->overflow = true;
            reply->abort();
            return;
        }
        body->bytes += reply->readAll();
    });
    return body;
}
bool finishBody(QNetworkReply* reply, const std::shared_ptr<Body>& body, int limit) {
    if (body->overflow || reply->bytesAvailable() > limit - body->bytes.size()) return false;
    body->bytes += reply->readAll();
    return body->bytes.size() <= limit && httpOk(reply);
}
QNetworkRequest boundedRequest(const QUrl& url, int timeoutMs) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(timeoutMs);
    return request;
}
bool parseService(const QByteArray& bytes, QString* serviceType, QString* controlPath) {
    QXmlStreamReader xml(bytes);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isDTD()) return false;
        if (!xml.isStartElement() || xml.name() != QStringLiteral("service")) continue;
        QString type, path;
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("serviceType")) type = xml.readElementText();
            else if (xml.name() == QStringLiteral("controlURL")) path = xml.readElementText();
            else xml.skipCurrentElement();
        }
        static const QRegularExpression supported(
            QStringLiteral(R"(^urn:schemas-upnp-org:service:WAN(?:IP|PPP)Connection:[12]$)"));
        if (supported.match(type).hasMatch() && !path.isEmpty()) {
            *serviceType = type;
            *controlPath = path;
            return !xml.hasError();
        }
    }
    return false;
}
QString parseExternalIp(const QByteArray& bytes) {
    QXmlStreamReader xml(bytes);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isDTD()) return {};
        if (xml.isStartElement() && xml.name() == QStringLiteral("NewExternalIPAddress")) {
            const QString address = xml.readElementText().trimmed();
            return xml.hasError() ? QString() : address;
        }
    }
    return {};
}
}

NetAddressDiscovery::NetAddressDiscovery(QObject* parent) : QObject(parent) {
    discoveryTimer_.setSingleShot(true);
    upnpTimer_.setSingleShot(true);
    routerNetwork_.setProxy(QNetworkProxy::NoProxy);
    connect(&socket_, &QUdpSocket::readyRead, this, &NetAddressDiscovery::readSsdp);
    connect(&discoveryTimer_, &QTimer::timeout, this, &NetAddressDiscovery::queryRouters);
    connect(&upnpTimer_, &QTimer::timeout, this, [this] { fallbackHttps(); });
}

bool NetAddressDiscovery::isPublicIpv4(const QString& text) {
    quint32 a = 0;
    if (!parseIpv4(text, &a)) return false;
    const auto first = a >> 24;
    if (first == 0 || first == 10 || first == 127 || first >= 224) return false;
    if ((a & 0xffc00000u) == 0x64400000u || // 100.64/10 shared address space
        (a & 0xfff00000u) == 0xac100000u || // 172.16/12
        (a & 0xffff0000u) == 0xa9fe0000u || // 169.254/16
        (a & 0xffff0000u) == 0xc0a80000u || // 192.168/16
        (a & 0xffffff00u) == 0xc0000000u || // 192.0.0/24 special use
        (a & 0xffffff00u) == 0xc0000200u || // TEST-NET-1
        (a & 0xffffff00u) == 0xc0586300u || // 192.88.99/24 deprecated relay
        (a & 0xfffe0000u) == 0xc6120000u || // 198.18/15 benchmark
        (a & 0xffffff00u) == 0xc6336400u || // TEST-NET-2
        (a & 0xffffff00u) == 0xcb007100u) return false; // TEST-NET-3
    return true;
}

void NetAddressDiscovery::start(const QString& connectionId) {
    if (active_ || attempted_) return;
    attempted_ = active_ = true;
    usingHttps_ = false;
    routers_.clear();
    routerReplies_ = 0;
    ++generation_;
    connection_ = NetInterfaces::choose(connectionId);
    if (connection_.id.isEmpty()) { finish({},QStringLiteral("No suitable Ethernet/Wi-Fi connection"));return; }
    if (!socket_.bind(QHostAddress(connection_.ipv4), 0)) { fallbackHttps(); return; }
    socket_.setMulticastInterface(QNetworkInterface::interfaceFromIndex(int(connection_.index)));
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 2);
    for (const char* version : {"1", "2"}) {
        const QByteArray search = QByteArray("M-SEARCH * HTTP/1.1\r\n") +
            "HOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n" +
            "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:" + version + "\r\n\r\n";
        socket_.writeDatagram(search, QHostAddress(QStringLiteral("239.255.255.250")), 1900);
    }
    discoveryTimer_.start(2000);
    upnpTimer_.start(4000);
}

void NetAddressDiscovery::cancel() {
    if (!active_ && !attempted_) return;
    active_ = attempted_ = false;
    ++generation_;
    discoveryTimer_.stop(); upnpTimer_.stop(); socket_.close();
    for (auto& reply : replies_) if (reply) { reply->abort(); reply->deleteLater(); }
    replies_.clear(); routers_.clear(); routerReplies_ = 0;
    if(publicRequest_) {auto request=publicRequest_;publicRequest_=nullptr;request->cancel();request->deleteLater();}
}

void NetAddressDiscovery::finish(const QString& address, const QString& detail) {
    if (!active_) return;
    active_ = false;
    ++generation_;
    discoveryTimer_.stop(); upnpTimer_.stop(); socket_.close();
    for (auto& reply : replies_) if (reply) { reply->abort(); reply->deleteLater(); }
    replies_.clear(); routers_.clear(); routerReplies_ = 0;
    if(publicRequest_) {auto request=publicRequest_;publicRequest_=nullptr;request->cancel();request->deleteLater();}
    emit finished(address, detail);
}

void NetAddressDiscovery::readSsdp() {
    int processed = 0;
    while (active_ && !usingHttps_ && socket_.hasPendingDatagrams() && processed++ < 32) {
        if (socket_.pendingDatagramSize() > 4096) {
            char ignored;
            socket_.readDatagram(&ignored, 1); // Discard the entire oversized datagram.
            continue;
        }
        const auto datagram = socket_.receiveDatagram(4096);
        const QString responder = datagram.senderAddress().toString();
        if(datagram.interfaceIndex() && datagram.interfaceIndex()!=connection_.index)continue;
        bool sameSubnet=false;
        for(const auto& entry:QNetworkInterface::interfaceFromIndex(int(connection_.index)).addressEntries())
            if(entry.ip()==QHostAddress(connection_.ipv4)&&entry.prefixLength()>=0&&
                datagram.senderAddress().isInSubnet(entry.ip(),entry.prefixLength())) {sameSubnet=true;break;}
        if(!sameSubnet)continue;
        if (!privateRouter(responder) || datagram.data().size() > 4096 ||
            !datagram.data().startsWith("HTTP/1.1 200")) continue;
        QByteArray location, service;
        for (QByteArray line : datagram.data().split('\n')) {
            line = line.trimmed();
            const int colon = line.indexOf(':');
            if (colon < 1) continue;
            const auto key = line.left(colon).trimmed().toLower();
            if (key == "location") location = line.mid(colon + 1).trimmed();
            if (key == "st") service = line.mid(colon + 1).trimmed();
        }
        if (!service.startsWith("urn:schemas-upnp-org:device:InternetGatewayDevice:") ||
            location.isEmpty() || location.size() > 2048) continue;
        const QUrl url = QUrl::fromEncoded(location, QUrl::StrictMode);
        if (!pinnedRouterUrl(url, responder)) continue;
        bool duplicate = false;
        for (const auto& router : routers_) if (router.description == url) duplicate = true;
        if (!duplicate && routers_.size() < MaxRouters) routers_.append({url, responder});
    }
    if (active_ && !usingHttps_ && socket_.hasPendingDatagrams()) {
        const int generation = generation_;
        QTimer::singleShot(0, this, [this,generation] {
            if (active_ && generation == generation_) readSsdp();
        });
    }
}

void NetAddressDiscovery::queryRouters() {
    socket_.close();
    if (routers_.isEmpty()) { fallbackHttps(); return; }
    for (const auto& router : routers_) fetchDescription(router);
}

void NetAddressDiscovery::fetchDescription(const Router& router) {
    ++routerReplies_;
    auto* reply = routerNetwork_.get(boundedRequest(router.description, 1800));
    replies_.append(reply);
    const auto body = collect(reply, this, DescriptionLimit);
    const int generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this,reply,body,router,generation] {
        if (generation != generation_ || !active_ || usingHttps_) { reply->deleteLater(); return; }
        QString type, path;
        if (finishBody(reply, body, DescriptionLimit) &&
            parseService(body->bytes, &type, &path)) {
            const QUrl control = router.description.resolved(QUrl(path));
            if (pinnedRouterUrl(control, router.responder, router.description.port(80)))
                queryExternalAddress(control, type);
        }
        reply->deleteLater();
        --routerReplies_;
        if (routerReplies_ == 0) fallbackHttps();
    });
}

void NetAddressDiscovery::queryExternalAddress(const QUrl& control,
                                                const QString& serviceType) {
    ++routerReplies_;
    const QByteArray type = serviceType.toLatin1();
    const QByteArray soap = QByteArray(
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:GetExternalIPAddress xmlns:u=\"") + type +
        "\"></u:GetExternalIPAddress></s:Body></s:Envelope>";
    auto request = boundedRequest(control, 1800);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("text/xml; charset=\"utf-8\""));
    request.setRawHeader("SOAPAction", '"' + type + "#GetExternalIPAddress\"");
    auto* reply = routerNetwork_.post(request, soap);
    replies_.append(reply);
    const auto body = collect(reply, this, SoapLimit);
    const int generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this,reply,body,generation] {
        if (generation != generation_ || !active_ || usingHttps_) { reply->deleteLater(); return; }
        if (finishBody(reply, body, SoapLimit)) {
            const QString address = parseExternalIp(body->bytes);
            if (isPublicIpv4(address)) { reply->deleteLater(); finish(address, QStringLiteral("UPnP router")); return; }
        }
        reply->deleteLater();
        --routerReplies_;
        if (routerReplies_ == 0) fallbackHttps();
    });
}

void NetAddressDiscovery::fallbackHttps(int serviceIndex) {
    if (!active_ || (usingHttps_ && serviceIndex == 0)) return;
    if (!usingHttps_) {
        usingHttps_ = true;
        discoveryTimer_.stop(); upnpTimer_.stop(); socket_.close();
        for (auto& reply : replies_) if (reply) { reply->abort(); reply->deleteLater(); }
        replies_.clear(); routerReplies_ = 0;
    }
    if (serviceIndex > 1) {
        finish({}, QStringLiteral("External IPv4 could not be determined"));
        return;
    }
    const QUrl url(serviceIndex == 0 ? QStringLiteral("https://api.ipify.org")
                                      : QStringLiteral("https://ipv4.icanhazip.com"));
    auto* request = new BoundHttpRequest(this);
    publicRequest_=request;
    const int generation=generation_;
    connect(request,&BoundHttpRequest::finished,this,[this,request,serviceIndex,generation](const QByteArray& body,const QString& error){
        if(publicRequest_==request)publicRequest_=nullptr;
        request->deleteLater();
        if(generation!=generation_||!active_)return;
        const QString address=QString::fromLatin1(body).trimmed();
        if(error.isEmpty()&&isPublicIpv4(address)) {
            finish(address,QStringLiteral("HTTPS on selected interface"));return;
        }
        fallbackHttps(serviceIndex+1);
    });
    request->start(url,connection_,2500);
}
