#pragma once
#include <cstdint>

namespace CartridgeLimits {
// Includes any copier header: the core reads the raw file before conversion.
inline constexpr std::int64_t MaxRomSize = 128LL * 1024 * 1024;
}
