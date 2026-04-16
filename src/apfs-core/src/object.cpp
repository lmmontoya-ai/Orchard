#include "orchard/apfs/object.h"

#include <limits>

namespace orchard::apfs {
namespace {

PhysicalBlockCacheStats::AccessStats& SelectAccessStats(PhysicalBlockCacheStats& stats,
                                                        const PhysicalBlockAccessKind access_kind) {
  switch (access_kind) {
  case PhysicalBlockAccessKind::kMetadata:
    return stats.metadata;
  case PhysicalBlockAccessKind::kFileData:
    return stats.file_data;
  }

  return stats.metadata;
}

void RecordRequestedBlocks(PhysicalBlockCacheStats& stats,
                           const PhysicalBlockAccessKind access_kind, const std::size_t block_count,
                           const std::uint32_t block_size) {
  auto& access_stats = SelectAccessStats(stats, access_kind);
  const auto block_count_u64 = static_cast<std::uint64_t>(block_count);
  const auto requested_bytes = block_count_u64 * static_cast<std::uint64_t>(block_size);
  stats.requested_blocks += block_count_u64;
  stats.requested_bytes += requested_bytes;
  access_stats.requested_blocks += block_count_u64;
  access_stats.requested_bytes += requested_bytes;
}

void RecordCacheHit(PhysicalBlockCacheStats& stats, const PhysicalBlockAccessKind access_kind) {
  auto& access_stats = SelectAccessStats(stats, access_kind);
  ++stats.cache_hits;
  ++access_stats.cache_hits;
}

void RecordCacheMiss(PhysicalBlockCacheStats& stats, const PhysicalBlockAccessKind access_kind) {
  auto& access_stats = SelectAccessStats(stats, access_kind);
  ++stats.cache_misses;
  ++access_stats.cache_misses;
}

void RecordDeviceRead(PhysicalBlockCacheStats& stats, const PhysicalBlockAccessKind access_kind,
                      const std::size_t block_count, const std::uint32_t block_size) {
  auto& access_stats = SelectAccessStats(stats, access_kind);
  const auto block_count_u64 = static_cast<std::uint64_t>(block_count);
  const auto fetched_bytes = block_count_u64 * static_cast<std::uint64_t>(block_size);
  ++stats.device_reads;
  ++access_stats.device_reads;
  stats.fetched_blocks += block_count_u64;
  stats.fetched_bytes += fetched_bytes;
  access_stats.fetched_blocks += block_count_u64;
  access_stats.fetched_bytes += fetched_bytes;
}

} // namespace

blockio::Result<ObjectHeader> ParseObjectHeader(const std::span<const std::uint8_t> block) {
  if (!HasRange(block, 0U, kApfsObjectHeaderSize)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "APFS object block is too small for the common object header.");
  }

  ObjectHeader header;
  header.checksum = ReadLe64(block, 0x00U);
  header.oid = ReadLe64(block, 0x08U);
  header.xid = ReadLe64(block, 0x10U);
  header.raw_type = ReadLe32(block, 0x18U);
  header.raw_subtype = ReadLe32(block, 0x1CU);
  header.type = header.raw_type & kObjectTypeMask;
  header.subtype = header.raw_subtype & kObjectTypeMask;
  return header;
}

PhysicalBlockCache::PhysicalBlockCache(const std::size_t capacity_blocks)
    : capacity_blocks_(capacity_blocks) {
  stats_.capacity_blocks = capacity_blocks_;
}

void PhysicalBlockCache::Touch(
    const std::unordered_map<std::uint64_t, CacheEntry>::iterator entry) const {
  lru_order_.splice(lru_order_.begin(), lru_order_, entry->second.lru_position);
  entry->second.lru_position = lru_order_.begin();
}

void PhysicalBlockCache::TrimToCapacity() const {
  while (entries_.size() > capacity_blocks_ && !lru_order_.empty()) {
    const auto block_index = lru_order_.back();
    lru_order_.pop_back();
    entries_.erase(block_index);
  }
  stats_.cached_blocks = entries_.size();
}

blockio::Result<std::shared_ptr<const std::vector<std::uint8_t>>>
PhysicalBlockCache::ReadBlock(const blockio::Reader& reader,
                              const std::uint64_t container_byte_offset,
                              const std::uint32_t block_size, const std::uint64_t block_index,
                              const PhysicalBlockAccessKind access_kind) const {
  if (capacity_blocks_ == 0U) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Physical block cache is not configured with any capacity.");
  }

  {
    std::scoped_lock lock(mutex_);
    RecordRequestedBlocks(stats_, access_kind, 1U, block_size);
    const auto existing = entries_.find(block_index);
    if (existing != entries_.end()) {
      RecordCacheHit(stats_, access_kind);
      Touch(existing);
      return existing->second.bytes;
    }
    RecordCacheMiss(stats_, access_kind);
  }

  constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
  if (block_index > (kMaxUint64 - container_byte_offset) / block_size) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical object read would overflow the container byte range.");
  }

  const auto byte_offset = container_byte_offset + (block_index * block_size);
  auto block_bytes_result =
      blockio::ReadExact(reader, blockio::ReadRequest{.offset = byte_offset, .size = block_size});
  if (!block_bytes_result.ok()) {
    return block_bytes_result.error();
  }

  auto cached_bytes =
      std::make_shared<const std::vector<std::uint8_t>>(std::move(block_bytes_result.value()));
  {
    std::scoped_lock lock(mutex_);
    const auto existing = entries_.find(block_index);
    if (existing != entries_.end()) {
      RecordCacheHit(stats_, access_kind);
      Touch(existing);
      return existing->second.bytes;
    }

    RecordDeviceRead(stats_, access_kind, 1U, block_size);
    lru_order_.push_front(block_index);
    entries_.insert_or_assign(block_index, CacheEntry{
                                               .bytes = cached_bytes,
                                               .lru_position = lru_order_.begin(),
                                           });
    TrimToCapacity();
  }

  return cached_bytes;
}

blockio::Result<std::vector<std::uint8_t>> PhysicalBlockCache::ReadBlocks(
    const blockio::Reader& reader, const std::uint64_t container_byte_offset,
    const std::uint32_t block_size, const std::uint64_t first_block_index,
    const std::size_t block_count, const PhysicalBlockAccessKind access_kind) const {
  if (capacity_blocks_ == 0U) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Physical block cache is not configured with any capacity.");
  }
  if (block_count == 0U) {
    return std::vector<std::uint8_t>{};
  }

  constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
  if (first_block_index > kMaxUint64 - static_cast<std::uint64_t>(block_count - 1U)) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical block range would overflow the container block range.");
  }
  if (block_count > (std::numeric_limits<std::size_t>::max() / block_size)) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical block range exceeds the supported in-memory size.");
  }

  std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> cached_blocks(block_count);
  {
    std::scoped_lock lock(mutex_);
    RecordRequestedBlocks(stats_, access_kind, block_count, block_size);
    for (std::size_t index = 0; index < block_count; ++index) {
      const auto block_index = first_block_index + static_cast<std::uint64_t>(index);
      const auto existing = entries_.find(block_index);
      if (existing != entries_.end()) {
        RecordCacheHit(stats_, access_kind);
        Touch(existing);
        cached_blocks[index] = existing->second.bytes;
      } else {
        RecordCacheMiss(stats_, access_kind);
      }
    }
  }

  std::size_t current = 0U;
  while (current < block_count) {
    if (cached_blocks[current]) {
      ++current;
      continue;
    }

    const auto run_start = current;
    while (current < block_count && !cached_blocks[current]) {
      ++current;
    }
    const auto run_block_count = current - run_start;
    const auto run_first_block_index = first_block_index + static_cast<std::uint64_t>(run_start);
    if (run_first_block_index > (kMaxUint64 - container_byte_offset) / block_size) {
      return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                           "Physical block read would overflow the container byte range.");
    }

    const auto byte_offset = container_byte_offset + (run_first_block_index * block_size);
    const auto byte_size = run_block_count * static_cast<std::size_t>(block_size);
    auto range_bytes_result =
        blockio::ReadExact(reader, blockio::ReadRequest{.offset = byte_offset, .size = byte_size});
    if (!range_bytes_result.ok()) {
      return range_bytes_result.error();
    }

    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> inserted_blocks(run_block_count);
    for (std::size_t run_index = 0; run_index < run_block_count; ++run_index) {
      const auto slice_offset = run_index * static_cast<std::size_t>(block_size);
      std::vector<std::uint8_t> block_bytes(block_size, 0U);
      std::copy_n(range_bytes_result.value().begin() + static_cast<std::ptrdiff_t>(slice_offset),
                  static_cast<std::ptrdiff_t>(block_size), block_bytes.begin());
      inserted_blocks[run_index] =
          std::make_shared<const std::vector<std::uint8_t>>(std::move(block_bytes));
    }

    {
      std::scoped_lock lock(mutex_);
      RecordDeviceRead(stats_, access_kind, run_block_count, block_size);
      for (std::size_t run_index = 0; run_index < run_block_count; ++run_index) {
        const auto block_index = run_first_block_index + static_cast<std::uint64_t>(run_index);
        const auto existing = entries_.find(block_index);
        if (existing != entries_.end()) {
          RecordCacheHit(stats_, access_kind);
          Touch(existing);
          cached_blocks[run_start + run_index] = existing->second.bytes;
          continue;
        }

        lru_order_.push_front(block_index);
        auto cached_bytes = inserted_blocks[run_index];
        const auto [entry, inserted] =
            entries_.insert_or_assign(block_index, CacheEntry{
                                                       .bytes = std::move(cached_bytes),
                                                       .lru_position = lru_order_.begin(),
                                                   });
        cached_blocks[run_start + run_index] = entry->second.bytes;
      }
      TrimToCapacity();
    }
  }

  std::vector<std::uint8_t> bytes(block_count * static_cast<std::size_t>(block_size), 0U);
  for (std::size_t index = 0; index < block_count; ++index) {
    const auto destination_offset = index * static_cast<std::size_t>(block_size);
    std::copy(cached_blocks[index]->begin(), cached_blocks[index]->end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(destination_offset));
  }

  return bytes;
}

PhysicalBlockCacheStats PhysicalBlockCache::stats() const {
  std::scoped_lock lock(mutex_);
  return stats_;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PhysicalObjectReader::PhysicalObjectReader(const blockio::Reader& reader,
                                           const std::uint64_t container_byte_offset,
                                           const std::uint32_t block_size,
                                           PhysicalBlockCacheHandle block_cache)
    : reader_(&reader), container_byte_offset_(container_byte_offset), block_size_(block_size),
      block_cache_(std::move(block_cache)) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

blockio::Result<ObjectBlock>
PhysicalObjectReader::ReadPhysicalObject(const std::uint64_t block_index) const {
  if (reader_ == nullptr || block_size_ == 0U) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Physical object reader is not configured with a valid APFS block size.");
  }

  constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
  if (block_index > (kMaxUint64 - container_byte_offset_) / block_size_) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical object read would overflow the container byte range.");
  }

  std::vector<std::uint8_t> block_bytes;
  if (block_cache_) {
    auto cached_block_result =
        block_cache_->ReadBlock(*reader_, container_byte_offset_, block_size_, block_index,
                                PhysicalBlockAccessKind::kMetadata);
    if (!cached_block_result.ok()) {
      return cached_block_result.error();
    }
    block_bytes = *cached_block_result.value();
  } else {
    const auto byte_offset = container_byte_offset_ + (block_index * block_size_);
    auto block_bytes_result = blockio::ReadExact(
        *reader_, blockio::ReadRequest{.offset = byte_offset, .size = block_size_});
    if (!block_bytes_result.ok()) {
      return block_bytes_result.error();
    }
    block_bytes = std::move(block_bytes_result.value());
  }

  auto header_result = ParseObjectHeader(block_bytes);
  if (!header_result.ok()) {
    return header_result.error();
  }

  ObjectBlock object;
  object.block_index = block_index;
  object.header = header_result.value();
  object.bytes = std::move(block_bytes);
  return object;
}

} // namespace orchard::apfs
