#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

#include "orchard/blockio/error.h"
#include "orchard/blockio/reader.h"
#include "orchard_test/test.h"

namespace {

void OpensRegularFilesAndReadsFromOffsets() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_blockio_reader.img";

  {
    std::ofstream output(temp_path, std::ios::binary);
    const std::array<std::uint8_t, 8> bytes{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  auto reader_result = orchard::blockio::OpenReader(temp_path);
  ORCHARD_TEST_REQUIRE(reader_result.ok());

  auto reader = std::move(reader_result).value();
  ORCHARD_TEST_REQUIRE(reader->backend_name() == "windows_handle");

  const auto read_result =
      orchard::blockio::ReadExact(*reader, orchard::blockio::ReadRequest{.offset = 2U, .size = 4U});
  ORCHARD_TEST_REQUIRE(read_result.ok());
  ORCHARD_TEST_REQUIRE(read_result.value().size() == 4U);
  ORCHARD_TEST_REQUIRE(read_result.value()[0] == 0x30U);
  ORCHARD_TEST_REQUIRE(read_result.value()[3] == 0x60U);

  std::filesystem::remove(temp_path);
}

void MemoryReaderSupportsDeterministicReads() {
  std::vector<std::uint8_t> bytes{1U, 2U, 3U, 4U, 5U};
  auto reader = orchard::blockio::MakeMemoryReader(bytes, "memory-fixture");

  const auto read_result =
      orchard::blockio::ReadExact(*reader, orchard::blockio::ReadRequest{.offset = 1U, .size = 3U});
  ORCHARD_TEST_REQUIRE(read_result.ok());
  ORCHARD_TEST_REQUIRE(read_result.value().size() == 3U);
  ORCHARD_TEST_REQUIRE(read_result.value()[0] == 2U);
  ORCHARD_TEST_REQUIRE(read_result.value()[2] == 4U);
}

void ReadExactReturnsShortReadForOutOfRangeRequests() {
  auto reader =
      orchard::blockio::MakeMemoryReader(std::vector<std::uint8_t>{1U, 2U, 3U}, "short-read");

  const auto read_result =
      orchard::blockio::ReadExact(*reader, orchard::blockio::ReadRequest{.offset = 2U, .size = 4U});
  ORCHARD_TEST_REQUIRE(!read_result.ok());
  ORCHARD_TEST_REQUIRE(read_result.error().code == orchard::blockio::ErrorCode::kShortRead);
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"OpensRegularFilesAndReadsFromOffsets", &OpensRegularFilesAndReadsFromOffsets},
      {"MemoryReaderSupportsDeterministicReads", &MemoryReaderSupportsDeterministicReads},
      {"ReadExactReturnsShortReadForOutOfRangeRequests",
       &ReadExactReturnsShortReadForOutOfRangeRequests},
  });
}
