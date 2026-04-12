#include "orchard/apfs/probe.h"

#include <array>

namespace orchard::apfs {
namespace {

constexpr std::array<std::uint8_t, 4> kNxsbMagic{
  static_cast<std::uint8_t>('N'),
  static_cast<std::uint8_t>('X'),
  static_cast<std::uint8_t>('S'),
  static_cast<std::uint8_t>('B'),
};

}  // namespace

bool ProbeContainerMagic(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.size() < kNxsbMagic.size()) {
    return false;
  }

  for (std::size_t index = 0; index < kNxsbMagic.size(); ++index) {
    if (bytes[index] != kNxsbMagic[index]) {
      return false;
    }
  }

  return true;
}

}  // namespace orchard::apfs

