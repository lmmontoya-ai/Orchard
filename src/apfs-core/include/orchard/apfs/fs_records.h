#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "orchard/apfs/compression.h"
#include "orchard/apfs/fs_keys.h"

namespace orchard::apfs {

enum class InodeKind {
  kUnknown,
  kRegularFile,
  kDirectory,
  kSymlink,
};

struct InodeRecord {
  InodeKey key;
  std::uint64_t parent_id = 0;
  std::uint64_t logical_size = 0;
  std::uint64_t allocated_size = 0;
  std::uint64_t internal_flags = 0;
  std::uint64_t creation_time_unix_nanos = 0;
  std::uint64_t last_access_time_unix_nanos = 0;
  std::uint64_t last_write_time_unix_nanos = 0;
  std::uint64_t change_time_unix_nanos = 0;
  std::uint32_t child_count = 0;
  std::uint32_t link_count = 0;
  std::uint16_t mode = 0;
  InodeKind kind = InodeKind::kUnknown;
};

struct DirectoryEntryRecord {
  DirectoryRecordKey key;
  std::uint64_t file_id = 0;
  std::uint16_t flags = 0;
  InodeKind kind = InodeKind::kUnknown;
  std::optional<InodeRecord> inode;
};

struct FileExtentRecord {
  FileExtentKey key;
  std::uint64_t length = 0;
  std::uint64_t physical_block = 0;
  std::uint64_t flags = 0;
};

struct XattrRecord {
  XattrKey key;
  std::uint16_t flags = 0;
  std::vector<std::uint8_t> data;
};

struct FsTreeRecordView {
  std::span<const std::uint8_t> key;
  std::span<const std::uint8_t> value;
};

blockio::Result<InodeRecord> ParseInodeRecord(const FsTreeRecordView& record_view);
blockio::Result<DirectoryEntryRecord>
ParseDirectoryEntryRecord(const FsTreeRecordView& record_view);
blockio::Result<FileExtentRecord> ParseFileExtentRecord(const FsTreeRecordView& record_view);
blockio::Result<XattrRecord> ParseXattrRecord(const FsTreeRecordView& record_view);

bool IsDirectory(InodeKind kind) noexcept;
bool IsRegularFile(InodeKind kind) noexcept;
std::string_view ToString(InodeKind kind) noexcept;

} // namespace orchard::apfs
