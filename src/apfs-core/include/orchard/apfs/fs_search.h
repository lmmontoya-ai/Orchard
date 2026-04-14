#pragma once

#include <cstdint>
#include <span>

#include "orchard/apfs/btree.h"
#include "orchard/apfs/fs_keys.h"

namespace orchard::apfs {

[[nodiscard]] BtreeWalker::CompareFn MakeInodeLowerBoundCompare(std::uint64_t inode_id);
[[nodiscard]] BtreeWalker::CompareFn
MakeDirectoryRecordLowerBoundCompare(std::uint64_t directory_inode_id);
[[nodiscard]] BtreeWalker::CompareFn MakeFileExtentLowerBoundCompare(
    std::uint64_t inode_id, std::uint64_t logical_address = 0U);
[[nodiscard]] BtreeWalker::CompareFn MakeXattrLowerBoundCompare(std::uint64_t inode_id);

[[nodiscard]] blockio::Result<bool> IsInodeKeyFor(std::span<const std::uint8_t> key_bytes,
                                                  std::uint64_t inode_id);
[[nodiscard]] blockio::Result<bool>
IsDirectoryRecordKeyFor(std::span<const std::uint8_t> key_bytes, std::uint64_t directory_inode_id);
[[nodiscard]] blockio::Result<bool> IsFileExtentKeyFor(std::span<const std::uint8_t> key_bytes,
                                                       std::uint64_t inode_id);
[[nodiscard]] blockio::Result<bool> IsXattrKeyFor(std::span<const std::uint8_t> key_bytes,
                                                  std::uint64_t inode_id);

} // namespace orchard::apfs
