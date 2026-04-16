#include "orchard/apfs/fs_search.h"

namespace orchard::apfs {
namespace {

int CompareFsKeyPrefix(const FsKeyHeader& actual, const std::uint64_t object_id,
                       const FsRecordType type) {
  if (actual.object_id != object_id) {
    return actual.object_id < object_id ? -1 : 1;
  }
  if (actual.type != type) {
    return static_cast<std::uint8_t>(actual.type) < static_cast<std::uint8_t>(type) ? -1 : 1;
  }
  return 0;
}

} // namespace

BtreeWalker::CompareFn MakeInodeLowerBoundCompare(const std::uint64_t inode_id) {
  return [inode_id](const std::span<const std::uint8_t> key_bytes) -> blockio::Result<int> {
    auto key_result = ParseFsKeyHeader(key_bytes);
    if (!key_result.ok()) {
      return key_result.error();
    }
    return CompareFsKeyPrefix(key_result.value(), inode_id, FsRecordType::kInode);
  };
}

BtreeWalker::CompareFn
MakeDirectoryRecordLowerBoundCompare(const std::uint64_t directory_inode_id) {
  return [directory_inode_id](
             const std::span<const std::uint8_t> key_bytes) -> blockio::Result<int> {
    auto key_result = ParseDirectoryRecordKey(key_bytes);
    if (key_result.ok()) {
      if (key_result.value().header.object_id != directory_inode_id) {
        return key_result.value().header.object_id < directory_inode_id ? -1 : 1;
      }
      return key_result.value().name.empty() ? 0 : 1;
    }

    if (key_result.error().code != blockio::ErrorCode::kCorruptData) {
      return key_result.error();
    }

    auto header_result = ParseFsKeyHeader(key_bytes);
    if (!header_result.ok()) {
      return header_result.error();
    }
    return CompareFsKeyPrefix(header_result.value(), directory_inode_id, FsRecordType::kDirRecord);
  };
}

BtreeWalker::CompareFn MakeFileExtentLowerBoundCompare(const std::uint64_t inode_id,
                                                       const std::uint64_t logical_address) {
  return [inode_id,
          logical_address](const std::span<const std::uint8_t> key_bytes) -> blockio::Result<int> {
    auto key_result = ParseFileExtentKey(key_bytes);
    if (key_result.ok()) {
      if (key_result.value().header.object_id != inode_id) {
        return key_result.value().header.object_id < inode_id ? -1 : 1;
      }
      if (key_result.value().header.type != FsRecordType::kFileExtent) {
        return static_cast<std::uint8_t>(key_result.value().header.type) <
                       static_cast<std::uint8_t>(FsRecordType::kFileExtent)
                   ? -1
                   : 1;
      }
      if (key_result.value().logical_address != logical_address) {
        return key_result.value().logical_address < logical_address ? -1 : 1;
      }
      return 0;
    }

    if (key_result.error().code != blockio::ErrorCode::kCorruptData) {
      return key_result.error();
    }

    auto header_result = ParseFsKeyHeader(key_bytes);
    if (!header_result.ok()) {
      return header_result.error();
    }
    return CompareFsKeyPrefix(header_result.value(), inode_id, FsRecordType::kFileExtent);
  };
}

BtreeWalker::CompareFn MakeXattrLowerBoundCompare(const std::uint64_t inode_id) {
  return [inode_id](const std::span<const std::uint8_t> key_bytes) -> blockio::Result<int> {
    auto key_result = ParseXattrKey(key_bytes);
    if (key_result.ok()) {
      if (key_result.value().header.object_id != inode_id) {
        return key_result.value().header.object_id < inode_id ? -1 : 1;
      }
      return key_result.value().name.empty() ? 0 : 1;
    }

    if (key_result.error().code != blockio::ErrorCode::kCorruptData) {
      return key_result.error();
    }

    auto header_result = ParseFsKeyHeader(key_bytes);
    if (!header_result.ok()) {
      return header_result.error();
    }
    return CompareFsKeyPrefix(header_result.value(), inode_id, FsRecordType::kXattr);
  };
}

blockio::Result<bool> IsInodeKeyFor(const std::span<const std::uint8_t> key_bytes,
                                    const std::uint64_t inode_id) {
  auto key_result = ParseInodeKey(key_bytes);
  if (!key_result.ok()) {
    return key_result.error();
  }
  return key_result.value().header.object_id == inode_id;
}

blockio::Result<bool> IsDirectoryRecordKeyFor(const std::span<const std::uint8_t> key_bytes,
                                              const std::uint64_t directory_inode_id) {
  auto key_result = ParseFsKeyHeader(key_bytes);
  if (!key_result.ok()) {
    return key_result.error();
  }
  return key_result.value().object_id == directory_inode_id &&
         key_result.value().type == FsRecordType::kDirRecord;
}

blockio::Result<bool> IsFileExtentKeyFor(const std::span<const std::uint8_t> key_bytes,
                                         const std::uint64_t inode_id) {
  auto key_result = ParseFsKeyHeader(key_bytes);
  if (!key_result.ok()) {
    return key_result.error();
  }
  return key_result.value().object_id == inode_id &&
         key_result.value().type == FsRecordType::kFileExtent;
}

blockio::Result<bool> IsXattrKeyFor(const std::span<const std::uint8_t> key_bytes,
                                    const std::uint64_t inode_id) {
  auto key_result = ParseFsKeyHeader(key_bytes);
  if (!key_result.ok()) {
    return key_result.error();
  }
  return key_result.value().object_id == inode_id &&
         key_result.value().type == FsRecordType::kXattr;
}

} // namespace orchard::apfs
