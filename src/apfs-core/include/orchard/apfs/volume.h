#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "orchard/apfs/discovery.h"
#include "orchard/apfs/fs_records.h"
#include "orchard/apfs/omap.h"

namespace orchard::apfs {

struct VolumeRuntimeConfig {
  std::uint64_t container_byte_offset = 0;
  std::uint32_t block_size = 0;
  std::uint64_t xid_limit = 0;
};

struct PhysicalReadRequest {
  std::uint64_t physical_block_index = 0;
  std::uint64_t block_offset = 0;
  std::size_t size = 0;
};

struct VolumePerformanceStats {
  std::uint64_t path_lookup_calls = 0;
  std::uint64_t path_components_walked = 0;
  std::uint64_t path_directory_enumerations = 0;
  std::uint64_t inode_cache_hits = 0;
  std::uint64_t inode_cache_misses = 0;
  PhysicalBlockCacheStats block_cache;
};

class VolumeContext {
public:
  static blockio::Result<VolumeContext> Load(const blockio::Reader& reader,
                                             const ContainerInfo& container, const VolumeInfo& info,
                                             const OmapResolver& container_omap);

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
  ReadPhysicalBytes(const PhysicalReadRequest& request) const;
  [[nodiscard]] VolumePerformanceStats performance_stats() const;
  void RecordPathLookup(std::size_t component_count, std::size_t directory_enumerations) const;

private:
  struct TreeBlockIndexes {
    std::uint64_t root_tree_block_index = 0;
    std::uint64_t volume_omap_block_index = 0;
  };

  struct PerformanceState {
    std::mutex mutex;
    std::uint64_t path_lookup_calls = 0;
    std::uint64_t path_components_walked = 0;
    std::uint64_t path_directory_enumerations = 0;
    std::uint64_t inode_cache_hits = 0;
    std::uint64_t inode_cache_misses = 0;
  };

  struct InodeCacheState {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, InodeRecord> entries;
  };

  VolumeContext(const blockio::Reader& reader, VolumeRuntimeConfig runtime, VolumeInfo info,
                TreeBlockIndexes tree_blocks);

  [[nodiscard]] PhysicalObjectReader MakeObjectReader() const;
  [[nodiscard]] blockio::Result<std::optional<NodeRecordCopy>>
  LowerBoundFilesystemRecord(const BtreeWalker::CompareFn& compare) const;
  [[nodiscard]] blockio::Result<std::size_t>
  VisitFilesystemRange(const BtreeWalker::CompareFn& compare,
                       const BtreeWalker::VisitFn& visitor) const;

  const blockio::Reader* reader_ = nullptr;
  std::uint64_t container_byte_offset_ = 0;
  std::uint32_t block_size_ = 0;
  std::uint64_t xid_limit_ = 0;
  VolumeInfo info_;
  std::uint64_t root_tree_block_index_ = 0;
  std::uint64_t volume_omap_block_index_ = 0;
  PhysicalBlockCacheHandle block_cache_;
  std::shared_ptr<PerformanceState> performance_;
  std::shared_ptr<InodeCacheState> inode_cache_;
};

} // namespace orchard::apfs
