#pragma once

#include <cstdint>
#include <span>

namespace orchard::apfs {

bool ProbeContainerMagic(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace orchard::apfs

