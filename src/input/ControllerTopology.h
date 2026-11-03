#pragma once

// Stable setting/recording identifiers. Physical adapters belong to the console,
// while logical player indices follow connected ports in hardware order.
enum class ControllerAdapter {
    None = 0, TeamPlayerPort1 = 1, TeamPlayerPort2 = 2, TeamPlayerBoth = 3,
    FourWayPlay = 4, MasterTapPort1 = 5, MasterTapPort2 = 6, MasterTapBoth = 7
};
constexpr unsigned MaxControllerPlayers = 8;
constexpr unsigned MaxGunPlayers = 3;
constexpr bool validControllerAdapter(int value) { return value >= 0 && value <= 7; }
constexpr bool isMasterTap(ControllerAdapter adapter) {
    return int(adapter) >= 5 && int(adapter) <= 7;
}
constexpr unsigned adapterPlayerCount(ControllerAdapter adapter) {
    switch (adapter) {
    case ControllerAdapter::TeamPlayerBoth: case ControllerAdapter::MasterTapBoth: return 8;
    case ControllerAdapter::TeamPlayerPort1: case ControllerAdapter::TeamPlayerPort2:
    case ControllerAdapter::MasterTapPort1: case ControllerAdapter::MasterTapPort2: return 5;
    case ControllerAdapter::FourWayPlay: return 4;
    default: return 2;
    }
}
