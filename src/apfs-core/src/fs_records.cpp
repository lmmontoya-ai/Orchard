#include "orchard/apfs/fs_records.h"

namespace orchard::apfs {
namespace {

constexpr std::uint16_t kModeTypeMask = 0xF000U;
constexpr std::uint16_t kModeDirectory = 0x4000U;
constexpr std::uint16_t kModeRegularFile = 0x8000U;
constexpr std::uint16_t kModeSymlink = 0xA000U;

InodeKind InferInodeKind(const std::uint16_t mode) {
  switch (mode & kModeTypeMask) {
  case kModeDirectory:
    return InodeKind::kDirectory;
  case kModeRegularFile:
    return InodeKind::kRegularFile;
  case kModeSymlink:
    return InodeKind::kSymlink;
  default:
    return InodeKind::kUnknown;
  }
}

} // namespace

blockio::Result<InodeRecord> ParseInodeRecord(const std::span<const std::uint8_t> key,
                                              const std::span<const std::uint8_t> value) {
  auto key_result = ParseInodeKey(key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(value, 0x58U, 8U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS inode value is missing required fixed fields.");
  }

  InodeRecord record;
  record.key = key_result.value();
  record.parent_id = ReadLe64(value, 0x00U);
  record.allocated_size = ReadLe64(value, 0x10U);
  record.internal_flags = ReadLe64(value, 0x18U);
  record.child_count = ReadLe32(value, 0x20U);
  record.mode = ReadLe16(value, 0x24U);
  record.logical_size = ReadLe64(value, 0x58U);
  record.kind = InferInodeKind(record.mode);
  return record;
}

blockio::Result<DirectoryEntryRecord>
ParseDirectoryEntryRecord(const std::span<const std::uint8_t> key,
                          const std::span<const std::uint8_t> value) {
  auto key_result = ParseDirectoryRecordKey(key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(value, 0x00U, 10U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS directory entry value is too small.");
  }

  return DirectoryEntryRecord{
      .key = key_result.value(),
      .file_id = ReadLe64(value, 0x00U),
      .flags = ReadLe16(value, 0x08U),
  };
}

blockio::Result<FileExtentRecord> ParseFileExtentRecord(const std::span<const std::uint8_t> key,
                                                        const std::span<const std::uint8_t> value) {
  auto key_result = ParseFileExtentKey(key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(value, 0x00U, 24U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS file extent value is too small.");
  }

  return FileExtentRecord{
      .key = key_result.value(),
      .length = ReadLe64(value, 0x00U),
      .physical_block = ReadLe64(value, 0x08U),
      .flags = ReadLe64(value, 0x10U),
  };
}

blockio::Result<XattrRecord> ParseXattrRecord(const std::span<const std::uint8_t> key,
                                              const std::span<const std::uint8_t> value) {
  auto key_result = ParseXattrKey(key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(value, 0x00U, 8U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS xattr value is too small.");
  }

  const auto data_length = ReadLe32(value, 0x04U);
  if (!HasRange(value, 0x08U, data_length)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS xattr payload extends beyond the record value.");
  }

  XattrRecord record;
  record.key = key_result.value();
  record.flags = ReadLe16(value, 0x00U);
  record.data.assign(value.begin() + 8,
                     value.begin() + 8 + static_cast<std::ptrdiff_t>(data_length));
  return record;
}

bool IsDirectory(const InodeKind kind) noexcept {
  return kind == InodeKind::kDirectory;
}

bool IsRegularFile(const InodeKind kind) noexcept {
  return kind == InodeKind::kRegularFile;
}

std::string_view ToString(const InodeKind kind) noexcept {
  switch (kind) {
  case InodeKind::kUnknown:
    return "unknown";
  case InodeKind::kRegularFile:
    return "regular_file";
  case InodeKind::kDirectory:
    return "directory";
  case InodeKind::kSymlink:
    return "symlink";
  }

  return "unknown";
}

} // namespace orchard::apfs
