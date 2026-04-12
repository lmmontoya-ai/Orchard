#include <array>
#include <vector>

#include "orchard/apfs/probe.h"
#include "orchard_test/test.h"

namespace {

void ExercisesProbeAcrossRepresentativeInputs() {
  const std::array<std::vector<std::uint8_t>, 4> samples{
    std::vector<std::uint8_t>{},
    std::vector<std::uint8_t>{static_cast<std::uint8_t>('N')},
    std::vector<std::uint8_t>{
      static_cast<std::uint8_t>('N'),
      static_cast<std::uint8_t>('X'),
      static_cast<std::uint8_t>('S'),
      static_cast<std::uint8_t>('B'),
    },
    std::vector<std::uint8_t>{
      static_cast<std::uint8_t>('B'),
      static_cast<std::uint8_t>('A'),
      static_cast<std::uint8_t>('D'),
      static_cast<std::uint8_t>('!'),
      0x00,
      0xff,
    },
  };

  for (const auto& sample : samples) {
    (void)orchard::apfs::ProbeContainerMagic(sample);
  }

  ORCHARD_TEST_REQUIRE(orchard::apfs::ProbeContainerMagic(samples[2]));
  ORCHARD_TEST_REQUIRE(!orchard::apfs::ProbeContainerMagic(samples[3]));
}

}  // namespace

int main() {
  return orchard_test::RunTests({
    {"ExercisesProbeAcrossRepresentativeInputs", &ExercisesProbeAcrossRepresentativeInputs},
  });
}

