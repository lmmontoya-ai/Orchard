#include "orchard/apfs/fs_records.h"

namespace orchard::apfs {
namespace {

constexpr std::uint16_t kModeTypeMask = 0xF000U;
constexpr std::uint16_t kModeDirectory = 0x4000U;
constexpr std::uint16_t kModeRegularFile = 0x8000U;
constexpr std::uint16_t kModeSymlink = 0xA000U;
constexpr std::size_t kRealInodeFixedSize = 0x5CU;
constexpr std::size_t kRealXattrHeaderSize = 4U;
constexpr std::size_t kSyntheticInodeLogicalSizeOffset = 0x58U;
constexpr std::size_t kSyntheticInodeMinimumSize = 0x60U;
constexpr std::size_t kSyntheticXattrMinimumSize = 8U;
constexpr std::size_t kSyntheticXattrDataOffset = 8U;
constexpr std::size_t kXfieldDescriptorSize = 4U;
constexpr std::uint8_t kInodeXfieldTypeDstream = 8U;
constexpr std::size_t kDstreamMinimumSize = 16U;

std::size_t AlignToEight(const std::size_t value) noexcept {
  return (value + 7U) & ~static_cast<std::size_t>(7U);
}

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

bool LooksLikeRealInodeValue(const std::span<const std::uint8_t> value) noexcept {
  if (!HasRange(value, 0x50U, 2U) || !HasRange(value, kRealInodeFixedSize, 4U)) {
    return false;
  }

  if (InferInodeKind(ReadLe16(value, 0x50U)) == InodeKind::kUnknown) {
    return false;
  }

  const auto xfield_count = ReadLe16(value, kRealInodeFixedSize + 0x00U);
  const auto xfield_data_bytes = ReadLe16(value, kRealInodeFixedSize + 0x02U);
  const auto descriptor_bytes = static_cast<std::size_t>(xfield_count) * kXfieldDescriptorSize;
  const auto descriptor_offset = kRealInodeFixedSize + 4U;
  if (!HasRange(value, descriptor_offset, descriptor_bytes)) {
    return false;
  }

  const auto data_offset = descriptor_offset + descriptor_bytes;
  return HasRange(value, data_offset, xfield_data_bytes);
}

blockio::Result<InodeRecord> ParseRealInodeRecord(const InodeKey& key,
                                                  const std::span<const std::uint8_t> value) {
  if (!LooksLikeRealInodeValue(value)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS inode value does not match the real on-disk inode layout.");
  }

  InodeRecord record;
  record.key = key;
  record.parent_id = ReadLe64(value, 0x00U);
  record.creation_time_unix_nanos = ReadLe64(value, 0x10U);
  record.last_write_time_unix_nanos = ReadLe64(value, 0x18U);
  record.change_time_unix_nanos = ReadLe64(value, 0x20U);
  record.last_access_time_unix_nanos = ReadLe64(value, 0x28U);
  record.internal_flags = ReadLe64(value, 0x30U);
  record.mode = ReadLe16(value, 0x50U);
  record.kind = InferInodeKind(record.mode);
  record.logical_size = ReadLe64(value, 0x54U);

  const auto children_or_links = ReadLe32(value, 0x38U);
  if (record.kind == InodeKind::kDirectory) {
    record.child_count = children_or_links;
  } else {
    record.link_count = children_or_links;
  }

  const auto xfield_count = ReadLe16(value, kRealInodeFixedSize + 0x00U);
  const auto xfield_data_bytes = ReadLe16(value, kRealInodeFixedSize + 0x02U);
  const auto descriptor_offset = kRealInodeFixedSize + 4U;
  const auto data_offset =
      descriptor_offset + (static_cast<std::size_t>(xfield_count) * kXfieldDescriptorSize);
  const auto data_limit = data_offset + static_cast<std::size_t>(xfield_data_bytes);

  std::size_t current_data_offset = data_offset;
  for (std::uint16_t index = 0; index < xfield_count; ++index) {
    const auto field_offset =
        descriptor_offset + (static_cast<std::size_t>(index) * kXfieldDescriptorSize);
    const auto field_type = value[field_offset + 0U];
    const auto field_size = static_cast<std::size_t>(ReadLe16(value, field_offset + 2U));

    if (!HasRange(value, current_data_offset, field_size) ||
        current_data_offset + field_size > data_limit) {
      return MakeApfsError(blockio::ErrorCode::kCorruptData,
                           "APFS inode xfield payload extends beyond the declared data area.");
    }

    if (field_type == kInodeXfieldTypeDstream) {
      if (field_size < kDstreamMinimumSize) {
        return MakeApfsError(blockio::ErrorCode::kCorruptData,
                             "APFS inode dstream xfield is truncated.");
      }

      record.logical_size = ReadLe64(value, current_data_offset + 0x00U);
      record.allocated_size = ReadLe64(value, current_data_offset + 0x08U);
    }

    current_data_offset += AlignToEight(field_size);
  }

  if (record.kind == InodeKind::kDirectory) {
    record.logical_size = 0U;
    record.allocated_size = 0U;
  } else if (record.allocated_size == 0U && record.logical_size != 0U) {
    record.allocated_size = record.logical_size;
  }

  return record;
}

blockio::Result<InodeRecord> ParseSyntheticInodeRecord(const InodeKey& key,
                                                       const std::span<const std::uint8_t> value) {
  if (!HasRange(value, kSyntheticInodeLogicalSizeOffset, 8U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS inode value is missing required fixed fields.");
  }

  InodeRecord record;
  record.key = key;
  record.parent_id = ReadLe64(value, 0x00U);
  record.allocated_size = ReadLe64(value, 0x10U);
  record.internal_flags = ReadLe64(value, 0x18U);
  record.child_count = ReadLe32(value, 0x20U);
  record.mode = ReadLe16(value, 0x24U);
  record.creation_time_unix_nanos = ReadLe64(value, 0x28U);
  record.last_access_time_unix_nanos = ReadLe64(value, 0x30U);
  record.last_write_time_unix_nanos = ReadLe64(value, 0x38U);
  record.change_time_unix_nanos = ReadLe64(value, 0x40U);
  record.link_count = ReadLe32(value, 0x48U);
  record.logical_size = ReadLe64(value, kSyntheticInodeLogicalSizeOffset);
  record.kind = InferInodeKind(record.mode);
  return record;
}

} // namespace

blockio::Result<InodeRecord> ParseInodeRecord(const FsTreeRecordView& record_view) {
  auto key_result = ParseInodeKey(record_view.key);
  if (!key_result.ok()) {
    return key_result.error();
  }

  if (LooksLikeRealInodeValue(record_view.value)) {
    return ParseRealInodeRecord(key_result.value(), record_view.value);
  }

  if (!HasRange(record_view.value, 0x00U, kSyntheticInodeMinimumSize)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS inode value is missing required fields.");
  }

  return ParseSyntheticInodeRecord(key_result.value(), record_view.value);
}

blockio::Result<DirectoryEntryRecord>
ParseDirectoryEntryRecord(const FsTreeRecordView& record_view) {
  auto key_result = ParseDirectoryRecordKey(record_view.key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(record_view.value, 0x00U, 10U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS directory entry value is too small.");
  }

  const auto flags_offset = HasRange(record_view.value, 0x10U, 2U) ? 0x10U : 0x08U;

  return DirectoryEntryRecord{
      .key = key_result.value(),
      .file_id = ReadLe64(record_view.value, 0x00U),
      .flags = ReadLe16(record_view.value, flags_offset),
  };
}

blockio::Result<FileExtentRecord> ParseFileExtentRecord(const FsTreeRecordView& record_view) {
  auto key_result = ParseFileExtentKey(record_view.key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(record_view.value, 0x00U, 24U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Synthetic APFS file extent value is too small.");
  }

  return FileExtentRecord{
      .key = key_result.value(),
      .length = ReadLe64(record_view.value, 0x00U),
      .physical_block = ReadLe64(record_view.value, 0x08U),
      .flags = ReadLe64(record_view.value, 0x10U),
  };
}

blockio::Result<XattrRecord> ParseXattrRecord(const FsTreeRecordView& record_view) {
  auto key_result = ParseXattrKey(record_view.key);
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (!HasRange(record_view.value, 0x00U, kRealXattrHeaderSize)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData, "APFS xattr value is too small.");
  }

  const auto flags = ReadLe16(record_view.value, 0x00U);
  const auto real_data_length = static_cast<std::size_t>(ReadLe16(record_view.value, 0x02U));
  const auto uses_real_layout =
      HasRange(record_view.value, kRealXattrHeaderSize, real_data_length) &&
      (record_view.value.size() < kSyntheticXattrMinimumSize || real_data_length != 0U ||
       (kRealXattrHeaderSize + real_data_length) == record_view.value.size());

  std::size_t data_offset = kRealXattrHeaderSize;
  std::size_t data_length = real_data_length;
  if (!uses_real_layout) {
    if (!HasRange(record_view.value, 0x00U, kSyntheticXattrMinimumSize)) {
      return MakeApfsError(blockio::ErrorCode::kCorruptData,
                           "APFS xattr value is too small for either supported layout.");
    }

    data_length = static_cast<std::size_t>(ReadLe32(record_view.value, 0x04U));
    data_offset = kSyntheticXattrDataOffset;
  }

  if (!HasRange(record_view.value, data_offset, data_length)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS xattr payload extends beyond the record value.");
  }

  XattrRecord record;
  record.key = key_result.value();
  record.flags = flags;
  record.data.assign(record_view.value.begin() + static_cast<std::ptrdiff_t>(data_offset),
                     record_view.value.begin() +
                         static_cast<std::ptrdiff_t>(data_offset + data_length));
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
