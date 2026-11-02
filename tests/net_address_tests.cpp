#include "net/NetAddressDiscovery.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // Explicit diagnostic only; default tests never contact the LAN or Internet.
    if(app.arguments().contains(QStringLiteral("--live"))) {
        NetAddressDiscovery discovery;
        QObject::connect(&discovery,&NetAddressDiscovery::finished,&app,[&](const QString& address,const QString& detail){
            std::fprintf(stderr,"Address discovery: %s (%s)\n",address.isEmpty()?"failed":"public IPv4 received",qPrintable(detail));
            app.exit(address.isEmpty()?1:0);
        });
        QTimer::singleShot(11000,&app,[&]{discovery.cancel();app.exit(2);});
        discovery.start();return app.exec();
    }
    for (const char* address : {"1.1.1.1", "8.8.8.8", "9.9.9.9", "192.0.1.1", "223.255.255.254"}) {
        if (!NetAddressDiscovery::isPublicIpv4(QString::fromLatin1(address))) {
            std::fprintf(stderr, "Rejected public IPv4: %s\n", address); return 1;
        }
    }
    for (const char* address : {"", "1.2.3", "1.2.3.4.5", "01.2.3.4", "256.1.1.1", "1.2.3.4 ",
                                "0.1.2.3", "10.2.3.4", "100.64.0.1", "100.127.255.254",
                                "127.0.0.1", "169.254.1.1", "172.16.0.1", "172.31.255.255",
                                "192.0.0.1", "192.0.2.1", "192.88.99.1", "192.168.1.1",
                                "198.18.0.1", "198.19.255.255", "198.51.100.1", "203.0.113.1",
                                "224.0.0.1", "239.255.255.250", "240.0.0.1", "255.255.255.255",
                                "::ffff:8.8.8.8", "localhost"}) {
        if (NetAddressDiscovery::isPublicIpv4(QString::fromLatin1(address))) {
            std::fprintf(stderr, "Accepted reserved IPv4: %s\n", address); return 1;
        }
    }
    std::puts("net address tests passed");
}
