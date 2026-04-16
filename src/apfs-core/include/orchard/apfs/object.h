#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "orchard/apfs/format.h"
#include "orchard/blockio/reader.h"

namespace orchard::apfs {

struct ObjectHeader {
  std::uint64_t checksum = 0;
  std::uint64_t oid = 0;
  std::uint64_t xid = 0;
  std::uint32_t raw_type = 0;
  std::uint32_t raw_subtype = 0;
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
};

struct ObjectBlock {
  std::uint64_t block_index = 0;
  ObjectHeader header;
  std::vector<std::uint8_t> bytes;

  [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
    return bytes;
  }
};

blockio::Result<ObjectHeader> ParseObjectHeader(std::span<const std::uint8_t> block);

struct PhysicalBlockCacheStats {
  struct AccessStats {
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t device_reads = 0;
    std::uint64_t requested_blocks = 0;
    std::uint64_t fetched_blocks = 0;
    std::uint64_t requested_bytes = 0;
    std::uint64_t fetched_bytes = 0;
  };

  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t device_reads = 0;
  std::uint64_t requested_blocks = 0;
  std::uint64_t fetched_blocks = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t fetched_bytes = 0;
  std::size_t cached_blocks = 0;
  std::size_t capacity_blocks = 0;
  AccessStats metadata;
  AccessStats file_data;
};

enum class PhysicalBlockAccessKind : std::uint8_t {
  kMetadata = 0,
  kFileData = 1,
};

class PhysicalBlockCache {
public:
  explicit PhysicalBlockCache(std::size_t capacity_blocks);

  [[nodiscard]] blockio::Result<std::shared_ptr<const std::vector<std::uint8_t>>>
  ReadBlock(const blockio::Reader& reader, std::uint64_t container_byte_offset,
            std::uint32_t block_size, std::uint64_t block_index,
            PhysicalBlockAccessKind access_kind) const;
  [[nodiscard]] blockio::Result<std::vector<std::uint8_t>>
  ReadBlocks(const blockio::Reader& reader, std::uint64_t container_byte_offset,
             std::uint32_t block_size, std::uint64_t first_block_index, std::size_t block_count,
             PhysicalBlockAccessKind access_kind) const;
  [[nodiscard]] PhysicalBlockCacheStats stats() const;

private:
  struct CacheEntry {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::list<std::uint64_t>::iterator lru_position;
  };

  void Touch(std::unordered_map<std::uint64_t, CacheEntry>::iterator entry) const;
  void TrimToCapacity() const;

  std::size_t capacity_blocks_ = 0;
  mutable std::mutex mutex_;
  mutable std::list<std::uint64_t> lru_order_;
  mutable std::unordered_map<std::uint64_t, CacheEntry> entries_;
  mutable PhysicalBlockCacheStats stats_;
};

using PhysicalBlockCacheHandle = std::shared_ptr<PhysicalBlockCache>;

class PhysicalObjectReader {
public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  PhysicalObjectReader(const blockio::Reader& reader, std::uint64_t container_byte_offset,
                       std::uint32_t block_size, PhysicalBlockCacheHandle block_cache = {});

  [[nodiscard]] blockio::Result<ObjectBlock> ReadPhysicalObject(std::uint64_t block_index) const;
  [[nodiscard]] std::uint64_t container_byte_offset() const noexcept {
    return container_byte_offset_;
  }
  [[nodiscard]] std::uint32_t block_size() const noexcept {
    return block_size_;
  }

private:
  const blockio::Reader* reader_ = nullptr;
  std::uint64_t container_byte_offset_ = 0;
  std::uint32_t block_size_ = 0;
  PhysicalBlockCacheHandle block_cache_;
};

} // namespace orchard::apfs
