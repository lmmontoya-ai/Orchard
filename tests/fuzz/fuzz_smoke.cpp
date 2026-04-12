#include <array>
#include <vector>

#include "orchard/apfs/btree.h"
#include "orchard/apfs/object.h"
#include "orchard/apfs/probe.h"
#include "orchard_test/test.h"

namespace {

void ExercisesProbeAcrossRepresentativeInputs() {
  const std::array<std::vector<std::uint8_t>, 4> samples{
      std::vector<std::uint8_t>{},
      std::vector<std::uint8_t>{static_cast<std::uint8_t>('N')},
      [] {
        std::vector<std::uint8_t> bytes(64U, 0U);
        bytes[orchard::apfs::kApfsObjectMagicOffset + 0U] = static_cast<std::uint8_t>('N');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 1U] = static_cast<std::uint8_t>('X');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 2U] = static_cast<std::uint8_t>('S');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 3U] = static_cast<std::uint8_t>('B');
        return bytes;
      }(),
      [] {
        std::vector<std::uint8_t> bytes(64U, 0U);
        bytes[orchard::apfs::kApfsObjectMagicOffset + 0U] = static_cast<std::uint8_t>('B');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 1U] = static_cast<std::uint8_t>('A');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 2U] = static_cast<std::uint8_t>('D');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 3U] = static_cast<std::uint8_t>('!');
        bytes[orchard::apfs::kApfsObjectMagicOffset + 4U] = 0x00;
        bytes[orchard::apfs::kApfsObjectMagicOffset + 5U] = 0xff;
        return bytes;
      }(),
  };

  for (const auto& sample : samples) {
    (void)orchard::apfs::ProbeContainerMagic(sample);
    (void)orchard::apfs::ParseObjectHeader(sample);
    if (sample.size() >= orchard::apfs::kApfsMinimumBlockSize) {
      (void)orchard::apfs::ParseNode(sample, orchard::apfs::kApfsMinimumBlockSize);
    }
  }

  ORCHARD_TEST_REQUIRE(orchard::apfs::ProbeContainerMagic(samples[2]));
  ORCHARD_TEST_REQUIRE(!orchard::apfs::ProbeContainerMagic(samples[3]));
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"ExercisesProbeAcrossRepresentativeInputs", &ExercisesProbeAcrossRepresentativeInputs},
  });
}
