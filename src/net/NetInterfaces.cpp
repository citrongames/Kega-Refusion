#include "NetInterfaces.h"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ipifcons.h>
#elif defined(Q_OS_LINUX)
#include <arpa/inet.h>
#include <cerrno>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {
constexpr int NoMetric = std::numeric_limits<int>::max();
struct Route { bool defaultGateway = false; int metric = NoMetric; };

bool usableAddress(const QHostAddress& address) {
    if (address.protocol() != QAbstractSocket::IPv4Protocol) return false;
    const quint32 a = address.toIPv4Address();
    return a && (a >> 24) != 127 && (a & 0xffff0000u) != 0xa9fe0000u &&
           (a >> 24) < 224 && a != 0xffffffffu;
}

#ifdef Q_OS_WIN
QHash<uint,Route> defaultRoutes() {
    QHash<uint,Route> result;
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table) return result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& row = table->Table[i];
        if (row.DestinationPrefix.Prefix.si_family != AF_INET ||
            row.DestinationPrefix.PrefixLength != 0 ||
            row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr != 0 ||
            row.NextHop.si_family != AF_INET ||
            row.NextHop.Ipv4.sin_addr.S_un.S_addr == 0 || row.Loopback) continue;
        MIB_IPINTERFACE_ROW interfaceRow{};
        InitializeIpInterfaceEntry(&interfaceRow);
        interfaceRow.Family = AF_INET;
        interfaceRow.InterfaceIndex = row.InterfaceIndex;
        const quint64 interfaceMetric = GetIpInterfaceEntry(&interfaceRow) == NO_ERROR
            ? quint64(interfaceRow.Metric) : 0;
        const quint64 total = quint64(row.Metric) + interfaceMetric;
        auto& route = result[row.InterfaceIndex];
        route.defaultGateway = true;
        route.metric = std::min(route.metric, int(std::min<quint64>(total, NoMetric)));
    }
    FreeMibTable(table);
    return result;
}
bool physicalAdapter(uint index, const QString&) {
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = index;
    if (GetIfEntry2(&row) != NO_ERROR) return false;
    return row.InterfaceAndOperStatusFlags.HardwareInterface &&
           !row.InterfaceAndOperStatusFlags.FilterInterface &&
           !row.InterfaceAndOperStatusFlags.EndPointInterface &&
           !row.InterfaceAndOperStatusFlags.NotMediaConnected &&
           (row.Type == IF_TYPE_ETHERNET_CSMACD || row.Type == IF_TYPE_IEEE80211);
}
#elif defined(Q_OS_LINUX)
QHash<QString,Route> defaultRoutes() {
    QHash<QString,Route> result;
    QFile routes(QStringLiteral("/proc/net/route"));
    if (!routes.open(QIODevice::ReadOnly)) return result;
    const auto lines = QString::fromLatin1(routes.read(64 * 1024)).split('\n');
    for (const QString& line : lines) {
        const auto columns = line.simplified().split(' ');
        if (columns.size() < 8) continue;
        bool destinationOk = false, gatewayOk = false, flagsOk = false,
             metricOk = false, maskOk = false;
        const auto destination = columns[1].toUInt(&destinationOk, 16);
        const auto gateway = columns[2].toUInt(&gatewayOk, 16);
        const auto flags = columns[3].toUInt(&flagsOk, 16);
        const auto metric = columns[6].toInt(&metricOk);
        const auto mask = columns[7].toUInt(&maskOk, 16);
        if (!destinationOk || !gatewayOk || !flagsOk || !metricOk ||
            !maskOk || destination != 0 || mask != 0 || gateway == 0 ||
            (flags & 0x3u) != 0x3u || metric < 0) continue;
        auto& route = result[columns[0]];
        route.defaultGateway = true;
        route.metric = std::min(route.metric, metric);
    }
    return result;
}
bool physicalAdapter(uint, const QString& name) {
    const QString root = QStringLiteral("/sys/class/net/") + name;
    QFile type(root + QStringLiteral("/type"));
    if (!type.open(QIODevice::ReadOnly) || type.read(16).trimmed() != "1") return false;
    return QFileInfo(root + QStringLiteral("/device")).isSymLink();
}
#endif
}

QList<NetInterfaces::Connection> NetInterfaces::available() {
    QList<Connection> result;
#ifdef Q_OS_WIN
    const auto routes = defaultRoutes();
#elif defined(Q_OS_LINUX)
    const auto routes = defaultRoutes();
#endif
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack) ||
            iface.index() <= 0 || iface.name().isEmpty()) continue;
        QString ipv4;
        for (const auto& address : iface.addressEntries()) {
            if (usableAddress(address.ip())) { ipv4 = address.ip().toString(); break; }
        }
        if (ipv4.isEmpty()) continue;
        Connection connection;
        connection.id = iface.name();
        connection.name = iface.humanReadableName();
        if (connection.name.isEmpty()) connection.name = connection.id;
        connection.ipv4 = ipv4;
        connection.index = uint(iface.index());
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
        connection.physical = physicalAdapter(connection.index, connection.id);
#ifdef Q_OS_WIN
        const auto route = routes.value(connection.index);
#else
        const auto route = routes.value(connection.id);
#endif
        connection.defaultRoute = route.defaultGateway;
        connection.metric = route.metric;
#endif
        result.append(std::move(connection));
    }
    std::sort(result.begin(), result.end(), [](const Connection& a, const Connection& b) {
        if (a.physical != b.physical) return a.physical > b.physical;
        if (a.defaultRoute != b.defaultRoute) return a.defaultRoute > b.defaultRoute;
        if (a.metric != b.metric) return a.metric < b.metric;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return result;
}

NetInterfaces::Connection NetInterfaces::chooseFrom(const QList<Connection>& connections,
                                                     const QString& id) {
    if (!id.isEmpty()) {
        for (const auto& connection : connections)
            if (connection.id == id && connection.index && !connection.ipv4.isEmpty()) return connection;
        return {};
    }
    const Connection* selected = nullptr;
    for (const auto& connection : connections) {
        if (!connection.physical || !connection.defaultRoute || !connection.index ||
            connection.ipv4.isEmpty()) continue;
        if (!selected || connection.metric < selected->metric ||
            (connection.metric == selected->metric && connection.id < selected->id))
            selected = &connection;
    }
    return selected ? *selected : Connection{};
}

NetInterfaces::Connection NetInterfaces::choose(const QString& id) {
    return chooseFrom(available(), id);
}

bool NetInterfaces::bindSocket(qintptr fd, const Connection& choice, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (fd < 0 || choice.id.isEmpty() || !choice.index ||
        !usableAddress(QHostAddress(choice.ipv4)))
        return fail(QStringLiteral("Invalid network connection selection"));
    const auto current = QNetworkInterface::interfaceFromName(choice.id);
    if (current.index() != int(choice.index) ||
        !current.flags().testFlag(QNetworkInterface::IsUp) ||
        !current.flags().testFlag(QNetworkInterface::IsRunning))
        return fail(QStringLiteral("Selected network interface is no longer available"));
    bool addressPresent = false;
    for (const auto& entry : current.addressEntries())
        if (entry.ip().toString() == choice.ipv4) addressPresent = true;
    if (!addressPresent)
        return fail(QStringLiteral("Selected IPv4 address has changed"));
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    sockaddr_in bound{};
#ifdef Q_OS_WIN
    int boundLength = sizeof(bound);
    if (getsockname(SOCKET(fd), reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0)
#else
    socklen_t boundLength = sizeof(bound);
    if (getsockname(int(fd), reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0)
#endif
        return fail(QStringLiteral("Could not verify socket source address"));
    if (bound.sin_family != AF_INET || bound.sin_addr.s_addr !=
        htonl(QHostAddress(choice.ipv4).toIPv4Address()))
        return fail(QStringLiteral("Socket is not bound to the selected IPv4 address"));
#endif
#ifdef Q_OS_WIN
    if (choice.index > 0x00ffffffu)
        return fail(QStringLiteral("Network interface index is out of range"));
    const DWORD index = htonl(DWORD(choice.index));
    if (setsockopt(SOCKET(fd), IPPROTO_IP, IP_UNICAST_IF,
                   reinterpret_cast<const char*>(&index), sizeof(index)) != 0)
        return fail(QStringLiteral("Could not bind socket to network interface (Winsock %1)")
                    .arg(WSAGetLastError()));
    return true;
#elif defined(Q_OS_LINUX)
    const quint32 index = htonl(quint32(choice.index));
    if (setsockopt(int(fd), IPPROTO_IP, IP_UNICAST_IF, &index, sizeof(index)) == 0)
        return true;
    const int reason = errno;
    if (reason == ENOPROTOOPT || reason == EOPNOTSUPP) {
        const QByteArray name = choice.id.toLocal8Bit();
        if (!name.isEmpty() && name.size() < IFNAMSIZ &&
            setsockopt(int(fd), SOL_SOCKET, SO_BINDTODEVICE,
                       name.constData(), socklen_t(name.size() + 1)) == 0) return true;
    }
    return fail(QStringLiteral("Could not bind socket to network interface (errno %1)")
                .arg(errno));
#else
    return fail(QStringLiteral("Binding to a selected network interface is unsupported"));
#endif
}
