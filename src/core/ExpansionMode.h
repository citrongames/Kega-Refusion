#pragma once

// Preferences apply to optional devices. Native Sega CD and 32X media always
// receive the hardware required to boot.
enum class ExpansionMode { Auto, Connected, Disconnected };

enum class Cd32xRamCart { Disabled = 0, MiB4 = 4, MiB8 = 8, MiB16 = 16 };

constexpr Cd32xRamCart cd32xRamCartFromMiB(int size) {
    switch (size) {
    case 4: return Cd32xRamCart::MiB4;
    case 8: return Cd32xRamCart::MiB8;
    case 16: return Cd32xRamCart::MiB16;
    default: return Cd32xRamCart::Disabled;
    }
}
