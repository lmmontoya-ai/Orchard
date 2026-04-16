#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

#include "orchard/blockio/error.h"
#include "orchard/blockio/reader.h"
#include "orchard_test/test.h"

namespace {

class PartialReader final : public orchard::blockio::Reader {
public:
  PartialReader(std::vector<std::uint8_t> bytes, const std::size_t max_chunk,
                std::filesystem::path label)
      : bytes_(std::move(bytes)), max_chunk_(max_chunk), label_(std::move(label)) {}

  [[nodiscard]] orchard::blockio::Result<std::uint64_t> size_bytes() const override {
    return static_cast<std::uint64_t>(bytes_.size());
  }

  [[nodiscard]] orchard::blockio::Result<std::size_t>
  ReadAt(const std::uint64_t offset, const std::span<std::uint8_t> buffer) const override {
    if (offset >= bytes_.size()) {
      return static_cast<std::size_t>(0U);
    }

    const auto available = bytes_.size() - static_cast<std::size_t>(offset);
    const auto chunk_size = std::min({available, buffer.size(), max_chunk_});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), chunk_size, buffer.begin());
    return chunk_size;
  }

  [[nodiscard]] std::string_view backend_name() const noexcept override {
    return "partial";
  }

  [[nodiscard]] orchard::blockio::TargetKind target_kind() const noexcept override {
    return orchard::blockio::TargetKind::kRegularFile;
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept override {
    return label_;
  }

private:
  std::vector<std::uint8_t> bytes_;
  std::size_t max_chunk_ = 0U;
  std::filesystem::path label_;
};

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

void ReadExactRetriesPartialReadsUntilTheRequestIsSatisfied() {
  PartialReader reader(std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U, 6U}, 2U, "partial-read");

  const auto read_result =
      orchard::blockio::ReadExact(reader, orchard::blockio::ReadRequest{.offset = 1U, .size = 4U});
  ORCHARD_TEST_REQUIRE(read_result.ok());
  ORCHARD_TEST_REQUIRE(read_result.value().size() == 4U);
  ORCHARD_TEST_REQUIRE(read_result.value()[0] == 2U);
  ORCHARD_TEST_REQUIRE(read_result.value()[1] == 3U);
  ORCHARD_TEST_REQUIRE(read_result.value()[2] == 4U);
  ORCHARD_TEST_REQUIRE(read_result.value()[3] == 5U);
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"OpensRegularFilesAndReadsFromOffsets", &OpensRegularFilesAndReadsFromOffsets},
      {"MemoryReaderSupportsDeterministicReads", &MemoryReaderSupportsDeterministicReads},
      {"ReadExactReturnsShortReadForOutOfRangeRequests",
       &ReadExactReturnsShortReadForOutOfRangeRequests},
      {"ReadExactRetriesPartialReadsUntilTheRequestIsSatisfied",
       &ReadExactRetriesPartialReadsUntilTheRequestIsSatisfied},
  });
}
