#include "net/NetInterfaces.h"
#include <QCoreApplication>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QList<NetInterfaces::Connection> adapters = {
        {"vpn-guid", "WireGuard", "10.14.0.2", 54, false, true, 1},
        {"ethernet-guid", "Ethernet", "192.168.1.10", 16, true, true, 25},
        {"wifi-guid", "Wi-Fi", "192.168.1.11", 17, true, true, 40},
        {"vmware-guid", "VMware", "192.168.200.1", 18, false, false, 1}
    };
    if (NetInterfaces::chooseFrom(adapters).id != "ethernet-guid" ||
        NetInterfaces::chooseFrom(adapters, "vpn-guid").id != "vpn-guid" ||
        !NetInterfaces::chooseFrom(adapters, "unknown").id.isEmpty() ||
        !NetInterfaces::chooseFrom({adapters[0], adapters[3]}).id.isEmpty()) {
        std::fputs("network interface selection failed\n", stderr);
        return 1;
    }
    std::puts("net interfaces tests passed");
}
