#pragma once
#include <cstdint>

// Inspected Steam Cube World executable used by this GameNative setup.
// Keep the identity guard narrow: unsupported builds must not use native RVAs.
namespace mobileBuild {
inline constexpr std::uint32_t timestamp = 0x5d93ab0d;
inline constexpr std::uint32_t imageSize = 0x58a000;
inline constexpr std::uintptr_t gamePointerRva = 0x551a80;
}
