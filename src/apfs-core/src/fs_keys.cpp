#include "orchard/apfs/fs_keys.h"

namespace orchard::apfs {
namespace {

constexpr std::uint64_t kFsKeyObjectIdMask = 0x0FFFFFFFFFFFFFFFULL;
constexpr std::uint64_t kFsKeyTypeShift = 60U;
constexpr std::uint32_t kDirRecordNameLengthMask = 0x000003FFU;

blockio::Result<std::string> ParseNamedKeyString(const std::span<const std::uint8_t> bytes,
                                                 const std::string_view label) {
  if (!HasRange(bytes, 8U, 2U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         std::string(label) +
                             " key is too small for the synthetic APFS name-length field.");
  }

  const auto length = ReadLe16(bytes, 8U);
  if (!HasRange(bytes, 10U, length)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         std::string(label) + " key name extends beyond the record bytes.");
  }

  return DecodeUtf8Name(bytes.subspan(10U, length));
}

blockio::Result<std::string>
ParseHashedDirectoryKeyString(const std::span<const std::uint8_t> bytes) {
  if (!HasRange(bytes, 8U, 4U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Directory record key is too small for the APFS hashed name field.");
  }

  const auto length = ReadLe32(bytes, 8U) & kDirRecordNameLengthMask;
  if (!HasRange(bytes, 12U, length)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Directory record key name extends beyond the record bytes.");
  }
  return DecodeUtf8Name(bytes.subspan(12U, length));
}

} // namespace

blockio::Result<FsKeyHeader> ParseFsKeyHeader(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < sizeof(std::uint64_t)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Filesystem-tree key is too small for the synthetic key header.");
  }

  const auto raw = ReadLe64(bytes, 0U);
  return FsKeyHeader{
      .object_id = raw & kFsKeyObjectIdMask,
      .type = static_cast<FsRecordType>((raw >> kFsKeyTypeShift) & 0x0FU),
  };
}

blockio::Result<InodeKey> ParseInodeKey(const std::span<const std::uint8_t> bytes) {
  auto header_result = ParseFsKeyHeader(bytes);
  if (!header_result.ok()) {
    return header_result.error();
  }
  if (header_result.value().type != FsRecordType::kInode || bytes.size() != sizeof(std::uint64_t)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Filesystem-tree inode key does not match the expected 8-byte shape.");
  }

  return InodeKey{.header = header_result.value()};
}

blockio::Result<DirectoryRecordKey>
ParseDirectoryRecordKey(const std::span<const std::uint8_t> bytes) {
  auto header_result = ParseFsKeyHeader(bytes);
  if (!header_result.ok()) {
    return header_result.error();
  }
  if (header_result.value().type != FsRecordType::kDirRecord) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Filesystem-tree directory record key advertises the wrong type.");
  }

  auto name_result = ParseNamedKeyString(bytes, "directory record");
  if (!name_result.ok() && bytes.size() >= 13U) {
    name_result = ParseHashedDirectoryKeyString(bytes);
  }
  if (!name_result.ok()) {
    return name_result.error();
  }

  return DirectoryRecordKey{
      .header = header_result.value(),
      .name = std::move(name_result.value()),
  };
}

blockio::Result<FileExtentKey> ParseFileExtentKey(const std::span<const std::uint8_t> bytes) {
  auto header_result = ParseFsKeyHeader(bytes);
  if (!header_result.ok()) {
    return header_result.error();
  }
  if (header_result.value().type != FsRecordType::kFileExtent || !HasRange(bytes, 8U, 8U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Filesystem-tree file extent key does not match the expected shape.");
  }

  return FileExtentKey{
      .header = header_result.value(),
      .logical_address = ReadLe64(bytes, 8U),
  };
}

blockio::Result<XattrKey> ParseXattrKey(const std::span<const std::uint8_t> bytes) {
  auto header_result = ParseFsKeyHeader(bytes);
  if (!header_result.ok()) {
    return header_result.error();
  }
  if (header_result.value().type != FsRecordType::kXattr) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Filesystem-tree xattr key advertises the wrong type.");
  }

  auto name_result = ParseNamedKeyString(bytes, "xattr");
  if (!name_result.ok()) {
    return name_result.error();
  }

  return XattrKey{
      .header = header_result.value(),
      .name = std::move(name_result.value()),
  };
}

std::string_view ToString(const FsRecordType type) noexcept {
  switch (type) {
  case FsRecordType::kUnknown:
    return "unknown";
  case FsRecordType::kInode:
    return "inode";
  case FsRecordType::kXattr:
    return "xattr";
  case FsRecordType::kFileExtent:
    return "file_extent";
  case FsRecordType::kDirRecord:
    return "dir_record";
  }

  return "unknown";
}

} // namespace orchard::apfs
