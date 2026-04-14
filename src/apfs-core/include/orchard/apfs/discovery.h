#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orchard/apfs/format.h"
#include "orchard/apfs/policy.h"
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

struct DirectoryEntrySample {
  std::string name;
  std::uint64_t inode_id = 0;
  std::string kind;
};

struct FileProbeInfo {
  std::string path;
  std::uint64_t inode_id = 0;
  std::uint64_t size_bytes = 0;
  std::string kind;
  std::uint32_t link_count = 0;
  std::string compression;
  bool sparse = false;
  std::optional<std::string> symlink_target;
  std::vector<std::string> aliases;
  std::string preview_utf8;
  std::string preview_hex;
};

struct VolumeInfo {
  std::uint64_t object_id = 0;
  std::uint64_t xid = 0;
  std::uint32_t filesystem_index = 0;
  std::string name;
  std::string uuid;
  FeatureFlags features;
  std::uint16_t role = 0;
  std::vector<std::string> role_names;
  std::uint32_t root_tree_type = 0;
  std::uint64_t omap_oid = 0;
  std::uint64_t root_tree_oid = 0;
  std::uint64_t extentref_tree_oid = 0;
  std::uint64_t fext_tree_oid = 0;
  std::uint64_t doc_id_tree_oid = 0;
  std::uint64_t security_tree_oid = 0;
  std::uint64_t root_directory_object_id = kApfsRootDirectoryObjectId;
  bool case_insensitive = false;
  bool snapshots_present = false;
  bool encryption_rolled = false;
  bool incomplete_restore = false;
  bool normalization_insensitive = false;
  bool sealed = false;
  PolicyDecision policy;
  std::vector<DirectoryEntrySample> root_entries;
  std::vector<FileProbeInfo> root_file_probes;
  std::vector<std::string> notes;
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
