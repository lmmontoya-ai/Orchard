#include <array>
#include <filesystem>
#include <fstream>

#include "orchard/apfs/inspection.h"
#include "orchard/apfs/probe.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard_test/test.h"

namespace {

void DetectsNxsbMagic() {
  const std::array<std::uint8_t, 4> header{
      static_cast<std::uint8_t>('N'),
      static_cast<std::uint8_t>('X'),
      static_cast<std::uint8_t>('S'),
      static_cast<std::uint8_t>('B'),
  };

  ORCHARD_TEST_REQUIRE(orchard::apfs::ProbeContainerMagic(header));
}

void RejectsNonApfsMagic() {
  const std::array<std::uint8_t, 4> header{
      static_cast<std::uint8_t>('B'),
      static_cast<std::uint8_t>('A'),
      static_cast<std::uint8_t>('D'),
      static_cast<std::uint8_t>('!'),
  };

  ORCHARD_TEST_REQUIRE(!orchard::apfs::ProbeContainerMagic(header));
}

void StubInspectionReportsMagicForRegularFiles() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_probe.img";

  {
    std::ofstream output(temp_path, std::ios::binary);
    output << "NXSB";
    output << "fixture";
  }

  const auto target_info = orchard::blockio::InspectTargetPath(temp_path);
  const auto result = orchard::apfs::RunStubInspection(target_info);

  ORCHARD_TEST_REQUIRE(result.status == orchard::apfs::ProbeStatus::kStubScanned);
  ORCHARD_TEST_REQUIRE(result.apfs_container_magic_present);
  ORCHARD_TEST_REQUIRE(result.suggested_mount_mode == "unknown");

  std::filesystem::remove(temp_path);
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"DetectsNxsbMagic", &DetectsNxsbMagic},
      {"RejectsNonApfsMagic", &RejectsNonApfsMagic},
      {"StubInspectionReportsMagicForRegularFiles", &StubInspectionReportsMagicForRegularFiles},
  });
}
