#include <filesystem>
#include <fstream>

#include "orchard/blockio/inspection_target.h"
#include "orchard_test/test.h"

namespace {

void DetectsRegularFiles() {
  const auto temp_path =
      std::filesystem::temp_directory_path() / "orchard_blockio_regular_file.img";

  {
    std::ofstream output(temp_path, std::ios::binary);
    output << "fixture";
  }

  const auto info = orchard::blockio::InspectTargetPath(temp_path);

  ORCHARD_TEST_REQUIRE(info.exists);
  ORCHARD_TEST_REQUIRE(info.probe_candidate);
  ORCHARD_TEST_REQUIRE(info.kind == orchard::blockio::TargetKind::kRegularFile);
  ORCHARD_TEST_REQUIRE(info.size_bytes.has_value());
  const auto size_bytes = info.size_bytes.value_or(0);
  ORCHARD_TEST_REQUIRE(size_bytes == 7U);

  std::filesystem::remove(temp_path);
}

void DetectsRawDevicePatterns() {
  const auto info =
      orchard::blockio::InspectTargetPath(std::filesystem::path(R"(\\.\PhysicalDrive3)"));

  ORCHARD_TEST_REQUIRE(!info.exists);
  ORCHARD_TEST_REQUIRE(info.probe_candidate);
  ORCHARD_TEST_REQUIRE(info.kind == orchard::blockio::TargetKind::kRawDevice);
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"DetectsRegularFiles", &DetectsRegularFiles},
      {"DetectsRawDevicePatterns", &DetectsRawDevicePatterns},
  });
}
