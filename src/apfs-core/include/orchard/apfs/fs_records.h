#pragma once

#include <cstdint>
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
  std::uint32_t child_count = 0;
  std::uint16_t mode = 0;
  InodeKind kind = InodeKind::kUnknown;
};

struct DirectoryEntryRecord {
  DirectoryRecordKey key;
  std::uint64_t file_id = 0;
  std::uint16_t flags = 0;
  InodeKind kind = InodeKind::kUnknown;
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

blockio::Result<InodeRecord> ParseInodeRecord(std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> value);
blockio::Result<DirectoryEntryRecord>
ParseDirectoryEntryRecord(std::span<const std::uint8_t> key, std::span<const std::uint8_t> value);
blockio::Result<FileExtentRecord> ParseFileExtentRecord(std::span<const std::uint8_t> key,
                                                        std::span<const std::uint8_t> value);
blockio::Result<XattrRecord> ParseXattrRecord(std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> value);

bool IsDirectory(InodeKind kind) noexcept;
bool IsRegularFile(InodeKind kind) noexcept;
std::string_view ToString(InodeKind kind) noexcept;

} // namespace orchard::apfs
