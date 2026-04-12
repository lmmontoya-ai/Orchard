#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orchard/apfs/format.h"
#include "orchard/blockio/reader.h"

namespace orchard::apfs {

enum class LayoutKind {
  kUnknown,
  kDirectContainer,
  kGuidPartitionTable,
};

enum class CheckpointSource { kMainSuperblock, kCheckpointDescriptorArea };

struct PartitionInfo {
  std::string type_guid;
  std::string unique_guid;
  std::string name;
  std::uint64_t first_lba = 0;
  std::uint64_t last_lba = 0;
  std::uint64_t byte_offset = 0;
  std::uint64_t byte_length = 0;
  bool is_apfs_partition = false;
};

struct CheckpointInfo {
  std::uint64_t block_index = 0;
  std::uint64_t xid = 0;
  CheckpointSource source = CheckpointSource::kMainSuperblock;
};

struct VolumeInfo {
  std::uint64_t object_id = 0;
  std::uint32_t filesystem_index = 0;
  std::string name;
  std::string uuid;
  FeatureFlags features;
  std::uint16_t role = 0;
  std::vector<std::string> role_names;
  bool case_insensitive = false;
  bool sealed = false;
};

struct ContainerInfo {
  LayoutKind source_layout = LayoutKind::kUnknown;
  std::uint64_t byte_offset = 0;
  std::uint64_t byte_length = 0;
  std::uint32_t block_size = 0;
  std::uint64_t block_count = 0;
  std::string uuid;
  FeatureFlags features;
  CheckpointInfo selected_checkpoint;
  std::uint64_t spaceman_oid = 0;
  std::uint64_t omap_oid = 0;
  std::uint64_t reaper_oid = 0;
  bool volumes_resolved_via_omap = false;
  std::vector<std::uint64_t> volume_object_ids;
  std::vector<VolumeInfo> volumes;
  std::optional<PartitionInfo> partition;
  std::vector<std::string> notes;
};

struct DiscoveryReport {
  LayoutKind layout = LayoutKind::kUnknown;
  std::optional<std::uint32_t> gpt_block_size;
  std::vector<PartitionInfo> partitions;
  std::vector<ContainerInfo> containers;
  std::vector<std::string> notes;
};

blockio::Result<DiscoveryReport> Discover(const blockio::Reader& reader);
std::string_view ToString(LayoutKind layout) noexcept;
std::string_view ToString(CheckpointSource source) noexcept;

} // namespace orchard::apfs
