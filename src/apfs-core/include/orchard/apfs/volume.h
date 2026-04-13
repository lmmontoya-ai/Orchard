#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "orchard/apfs/discovery.h"
#include "orchard/apfs/fs_records.h"
#include "orchard/apfs/omap.h"

namespace orchard::apfs {

class VolumeContext {
public:
  static blockio::Result<VolumeContext>
  Load(const blockio::Reader& reader, std::uint64_t container_byte_offset, std::uint32_t block_size,
       std::uint64_t xid_limit, const VolumeInfo& info, const OmapResolver& container_omap);

  [[nodiscard]] const VolumeInfo& info() const noexcept {
    return info_;
  }
  [[nodiscard]] std::uint32_t block_size() const noexcept {
    return block_size_;
  }
  [[nodiscard]] std::uint64_t xid_limit() const noexcept {
    return xid_limit_;
  }
  [[nodiscard]] std::uint64_t root_tree_block_index() const noexcept {
    return root_tree_block_index_;
  }
  [[nodiscard]] std::uint64_t root_directory_object_id() const noexcept {
    return info_.root_directory_object_id;
  }

  [[nodiscard]] blockio::Result<InodeRecord> GetInode(std::uint64_t inode_id) const;
  [[nodiscard]] blockio::Result<std::vector<DirectoryEntryRecord>>
  ListDirectoryEntries(std::uint64_t directory_inode_id) const;
  [[nodiscard]] blockio::Result<std::vector<FileExtentRecord>>
  ListFileExtents(std::uint64_t inode_id) const;
  [[nodiscard]] blockio::Result<std::optional<XattrRecord>> FindXattr(std::uint64_t inode_id,
                                                                      std::string_view name) const;
  [[nodiscard]] blockio::Result<std::vector<std::uint8_t>>
  ReadPhysicalBytes(std::uint64_t physical_block_index, std::uint64_t block_offset,
                    std::size_t size) const;

private:
  VolumeContext(const blockio::Reader& reader, std::uint64_t container_byte_offset,
                std::uint32_t block_size, std::uint64_t xid_limit, VolumeInfo info,
                std::uint64_t root_tree_block_index);

  [[nodiscard]] PhysicalObjectReader MakeObjectReader() const;
  [[nodiscard]] blockio::Result<std::size_t>
  VisitFilesystemRecords(const BtreeWalker::VisitFn& visitor) const;

  const blockio::Reader* reader_ = nullptr;
  std::uint64_t container_byte_offset_ = 0;
  std::uint32_t block_size_ = 0;
  std::uint64_t xid_limit_ = 0;
  VolumeInfo info_;
  std::uint64_t root_tree_block_index_ = 0;
};

} // namespace orchard::apfs
