#include "orchard/fs_winfsp/mount.h"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "orchard/apfs/omap.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/fs_winfsp/filesystem.h"
#include "orchard/fs_winfsp/path_bridge.h"

namespace orchard::fs_winfsp {
namespace {

constexpr std::size_t kReadAheadTriggerSequentialReads = 3U;
constexpr std::size_t kReadAheadMinFileBytes = 512U * 1024U;
constexpr std::size_t kReadAheadLargeFileBytes = 4U * 1024U * 1024U;
constexpr std::size_t kReadAheadMinRequestBytes = 64U * 1024U;
constexpr std::size_t kReadAheadMinBytes = 256U * 1024U;
constexpr std::size_t kReadAheadLargeFileMinBytes = 512U * 1024U;
constexpr std::size_t kReadAheadMaxBytes = 1U * 1024U * 1024U;
constexpr std::size_t kReadAheadGrowthFactor = 2U;
constexpr std::size_t kSmallReadRequestBytes = 16U * 1024U;

struct SelectedVolumeRef {
  std::size_t container_index = 0;
  std::size_t volume_index = 0;
};

bool IsVolumeMountableReadOnly(const orchard::apfs::PolicyDecision& policy,
                               const MountConfig& config) noexcept {
  using orchard::apfs::MountDisposition;

  switch (policy.action) {
  case MountDisposition::kMountReadOnly:
    return true;
  case MountDisposition::kMountReadWrite:
    return config.require_read_only_mount ? config.allow_downgrade_from_readwrite : true;
  case MountDisposition::kHide:
  case MountDisposition::kReject:
    return false;
  }

  return false;
}

blockio::Result<SelectedVolumeRef> SelectVolume(const orchard::apfs::DiscoveryReport& report,
                                                const MountConfig& config) {
  std::vector<SelectedVolumeRef> candidates;

  for (std::size_t container_index = 0; container_index < report.containers.size();
       ++container_index) {
    const auto& container = report.containers[container_index];
    for (std::size_t volume_index = 0; volume_index < container.volumes.size(); ++volume_index) {
      const auto& volume = container.volumes[volume_index];
      const auto matches_object_id =
          !config.selector.object_id.has_value() || volume.object_id == *config.selector.object_id;
      const auto matches_name =
          !config.selector.name.has_value() || volume.name == *config.selector.name;
      if (!matches_object_id || !matches_name) {
        continue;
      }
      if (!IsVolumeMountableReadOnly(volume.policy, config)) {
        std::ostringstream message;
        message << "Selected APFS volume '" << volume.name
                << "' is not mountable through the read-only WinFsp adapter.";
        return orchard::apfs::MakeApfsError(blockio::ErrorCode::kUnsupportedTarget, message.str());
      }

      candidates.push_back(SelectedVolumeRef{
          .container_index = container_index,
          .volume_index = volume_index,
      });
    }
  }

  if (candidates.empty()) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kNotFound,
                                        "No APFS volume matched the requested selection.");
  }
  if (!config.selector.object_id.has_value() && !config.selector.name.has_value() &&
      candidates.size() != 1U) {
    return orchard::apfs::MakeApfsError(
        blockio::ErrorCode::kInvalidArgument,
        "The target contains multiple mountable APFS volumes; select one explicitly.");
  }
  if (candidates.size() > 1U) {
    return orchard::apfs::MakeApfsError(
        blockio::ErrorCode::kInvalidArgument,
        "The volume selector matched multiple APFS volumes; refine the selection.");
  }

  return candidates.front();
}

blockio::Result<std::vector<std::uint8_t>> MakeDefaultSecurityDescriptor() {
  constexpr PCWSTR kReadOnlySddl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)";

  PSECURITY_DESCRIPTOR security_descriptor = nullptr;
  ULONG security_descriptor_size = 0;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kReadOnlySddl, SDDL_REVISION_1, &security_descriptor, &security_descriptor_size)) {
    return orchard::apfs::MakeApfsError(
        blockio::ErrorCode::kOpenFailed,
        "Failed to create the default WinFsp security descriptor for Orchard.");
  }

  std::vector<std::uint8_t> bytes(security_descriptor_size, 0U);
  std::memcpy(bytes.data(), security_descriptor, security_descriptor_size);
  ::LocalFree(security_descriptor);
  return bytes;
}

std::uint32_t ComputeVolumeSerialNumber(const orchard::apfs::VolumeInfo& volume) noexcept {
  constexpr std::uint32_t kFnvOffset = 2166136261U;
  constexpr std::uint32_t kFnvPrime = 16777619U;

  std::uint32_t hash = kFnvOffset;
  const auto mix_byte = [&hash](const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
  };

  for (const auto ch : volume.uuid) {
    mix_byte(static_cast<std::uint8_t>(ch));
  }
  for (unsigned shift = 0; shift < 64; shift += 8) {
    mix_byte(static_cast<std::uint8_t>((volume.object_id >> shift) & 0xFFU));
  }

  return hash;
}

std::string ComposeChildPath(const std::string_view parent, const std::string_view child) {
  if (parent == "/") {
    return std::string("/") + std::string(child);
  }

  std::string path(parent);
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(child);
  return path;
}

blockio::Result<std::vector<std::uint8_t>>
ReadRegularFileBytes(const orchard::apfs::VolumeContext& context,
                     const std::vector<orchard::apfs::FileExtentRecord>& extents,
                     const std::uint64_t range_start, const std::size_t size) {
  std::vector<std::uint8_t> bytes(size, 0U);
  const auto range_end = range_start + size;

  for (const auto& extent : extents) {
    const auto extent_start = extent.key.logical_address;
    const auto extent_end = extent_start + extent.length;
    if (extent_end <= range_start || extent_start >= range_end) {
      continue;
    }

    const auto copy_start = std::max<std::uint64_t>(range_start, extent_start);
    const auto copy_end = std::min<std::uint64_t>(range_end, extent_end);
    const auto extent_relative_offset = copy_start - extent_start;
    const auto destination_offset = copy_start - range_start;
    const auto read_size = static_cast<std::size_t>(copy_end - copy_start);

    const auto physical_byte_offset = extent_relative_offset;
    const auto physical_block =
        extent.physical_block + (physical_byte_offset / context.block_size());
    const auto block_offset = physical_byte_offset % context.block_size();
    auto data_result = context.ReadPhysicalBytes(orchard::apfs::PhysicalReadRequest{
        .physical_block_index = physical_block,
        .block_offset = block_offset,
        .size = read_size,
    });
    if (!data_result.ok()) {
      return data_result.error();
    }

    std::copy(data_result.value().begin(), data_result.value().end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(destination_offset));
  }

  return bytes;
}

orchard::apfs::FileMetadata
MakeMetadataFromInode(const orchard::apfs::InodeRecord& inode) noexcept {
  orchard::apfs::FileMetadata metadata;
  metadata.object_id = inode.key.header.object_id;
  metadata.logical_size = inode.logical_size;
  metadata.allocated_size = inode.allocated_size;
  metadata.internal_flags = inode.internal_flags;
  metadata.creation_time_unix_nanos = inode.creation_time_unix_nanos;
  metadata.last_access_time_unix_nanos = inode.last_access_time_unix_nanos;
  metadata.last_write_time_unix_nanos = inode.last_write_time_unix_nanos;
  metadata.change_time_unix_nanos = inode.change_time_unix_nanos;
  metadata.kind = inode.kind;
  metadata.child_count = inode.child_count;
  metadata.link_count = inode.link_count;
  metadata.mode = inode.mode;
  metadata.sparse = (inode.internal_flags & 0x00000200ULL) != 0U;
  return metadata;
}

constexpr std::size_t kSymlinkResolutionDepthLimit = 16U;

std::vector<std::string> SplitPosixPath(const std::string_view path) {
  std::vector<std::string> components;
  std::size_t current = 0U;
  while (current < path.size()) {
    while (current < path.size() && path[current] == '/') {
      ++current;
    }
    if (current >= path.size()) {
      break;
    }

    auto next = path.find('/', current);
    if (next == std::string_view::npos) {
      next = path.size();
    }
    components.emplace_back(path.substr(current, next - current));
    current = next;
  }
  return components;
}

std::string JoinPosixPath(const std::vector<std::string>& components) {
  if (components.empty()) {
    return "/";
  }

  std::string path;
  for (const auto& component : components) {
    path.push_back('/');
    path.append(component);
  }
  return path;
}

std::string ParentPosixPath(const std::string_view path) {
  auto components = SplitPosixPath(path);
  if (!components.empty()) {
    components.pop_back();
  }
  return JoinPosixPath(components);
}

std::string NormalizePosixTargetPath(const std::string_view anchor_directory,
                                     const std::string_view target) {
  std::vector<std::string> components;
  if (target.empty() || target.front() != '/') {
    components = SplitPosixPath(anchor_directory);
  }

  for (const auto& component : SplitPosixPath(target)) {
    if (component == ".") {
      continue;
    }
    if (component == "..") {
      if (!components.empty()) {
        components.pop_back();
      }
      continue;
    }

    components.push_back(component);
  }

  return JoinPosixPath(components);
}

blockio::Result<orchard::apfs::ResolvedPath>
ResolveSymlinkChainWithinVolume(const orchard::apfs::VolumeContext& volume,
                                const std::string_view anchor_directory,
                                const std::string_view target, std::size_t depth_remaining,
                                std::unordered_set<std::string>& visited) {
  if (depth_remaining == 0U) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kUnsupportedTarget,
                                        "APFS symlink resolution exceeded the depth limit.");
  }

  const auto candidate_path = NormalizePosixTargetPath(anchor_directory, target);
  if (!visited.emplace(candidate_path).second) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kUnsupportedTarget,
                                        "APFS symlink resolution encountered a cycle.");
  }

  auto lookup_result = orchard::apfs::LookupPath(volume, candidate_path);
  if (!lookup_result.ok()) {
    return lookup_result.error();
  }

  if (lookup_result.value().inode.kind != orchard::apfs::InodeKind::kSymlink) {
    return lookup_result.value();
  }

  auto target_result =
      orchard::apfs::ReadSymlinkTarget(volume, lookup_result.value().inode.key.header.object_id);
  if (!target_result.ok()) {
    return target_result.error();
  }

  return ResolveSymlinkChainWithinVolume(volume, ParentPosixPath(candidate_path),
                                         target_result.value(), depth_remaining - 1U, visited);
}

blockio::Result<orchard::apfs::ResolvedPath>
ResolveSymlinkWithinVolume(const orchard::apfs::VolumeContext& volume,
                           const std::string_view normalized_path,
                           const std::string_view symlink_target) {
  std::unordered_set<std::string> visited;
  visited.emplace(std::string(normalized_path));
  return ResolveSymlinkChainWithinVolume(volume, ParentPosixPath(normalized_path), symlink_target,
                                         kSymlinkResolutionDepthLimit, visited);
}

} // namespace

MountedVolume::MountedVolume(MountConfig config, blockio::InspectionTargetInfo target_info,
                             blockio::ReaderHandle reader, orchard::apfs::ContainerInfo container,
                             orchard::apfs::VolumeInfo volume, orchard::apfs::VolumeContext context,
                             std::vector<std::uint8_t> security_descriptor,
                             std::wstring volume_label, const std::uint32_t volume_serial_number)
    : config_(std::move(config)), target_info_(std::move(target_info)), reader_(std::move(reader)),
      container_(std::move(container)), volume_(std::move(volume)), context_(std::move(context)),
      security_descriptor_(std::move(security_descriptor)), volume_label_(std::move(volume_label)),
      volume_serial_number_(volume_serial_number) {}

blockio::Result<MountedVolumeHandle> MountedVolume::Open(const MountConfig& config) {
  auto target_info = blockio::InspectTargetPath(config.target_path);
  if (!target_info.probe_candidate) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kUnsupportedTarget,
                                        "The requested mount target is not a block I/O candidate.");
  }

  auto reader_result = blockio::OpenReader(target_info);
  if (!reader_result.ok()) {
    return reader_result.error();
  }

  auto discovery_result = orchard::apfs::Discover(*reader_result.value());
  if (!discovery_result.ok()) {
    return discovery_result.error();
  }

  auto selected_result = SelectVolume(discovery_result.value(), config);
  if (!selected_result.ok()) {
    return selected_result.error();
  }

  auto reader = std::move(reader_result.value());
  const auto& selected_container =
      discovery_result.value().containers[selected_result.value().container_index];
  const auto& selected_volume = selected_container.volumes[selected_result.value().volume_index];

  orchard::apfs::PhysicalObjectReader object_reader(*reader, selected_container.byte_offset,
                                                    selected_container.block_size);
  auto container_omap_result =
      orchard::apfs::OmapResolver::Load(object_reader, selected_container.omap_oid);
  if (!container_omap_result.ok()) {
    return container_omap_result.error();
  }

  auto context_result = orchard::apfs::VolumeContext::Load(
      *reader, selected_container, selected_volume, container_omap_result.value());
  if (!context_result.ok()) {
    return context_result.error();
  }

  auto security_descriptor_result = MakeDefaultSecurityDescriptor();
  if (!security_descriptor_result.ok()) {
    return security_descriptor_result.error();
  }

  auto label_result = Utf8ToWide(selected_volume.name);
  if (!label_result.ok()) {
    return label_result.error();
  }

  auto mounted_volume = MountedVolumeHandle(new MountedVolume(
      config, std::move(target_info), std::move(reader), selected_container, selected_volume,
      std::move(context_result.value()), std::move(security_descriptor_result.value()),
      std::move(label_result.value()), ComputeVolumeSerialNumber(selected_volume)));
  return mounted_volume;
}

blockio::Result<bool> MountedVolume::ProjectSymlinkNode(FileNode& node) const {
  if (node.metadata.kind != orchard::apfs::InodeKind::kSymlink) {
    return true;
  }

  if (!node.symlink_target.has_value()) {
    ++symlink_target_loads_;
    auto target_result = orchard::apfs::ReadSymlinkTarget(context_, node.inode_id);
    if (!target_result.ok()) {
      return target_result.error();
    }
    node.symlink_target = std::move(target_result.value());
  }

  auto resolved_target_result =
      ResolveSymlinkWithinVolume(context_, node.normalized_path, *node.symlink_target);
  if (resolved_target_result.ok()) {
    auto resolved_metadata_result = orchard::apfs::GetFileMetadata(
        context_, resolved_target_result.value().inode.key.header.object_id);
    if (!resolved_metadata_result.ok()) {
      return resolved_metadata_result.error();
    }
    if (orchard::apfs::IsRegularFile(resolved_metadata_result.value().kind)) {
      node.inode_id = resolved_target_result.value().inode.key.header.object_id;
      node.parent_inode_id = resolved_target_result.value().inode.parent_id;
      node.metadata = resolved_metadata_result.value();
    } else {
      node.metadata.kind = orchard::apfs::InodeKind::kRegularFile;
      node.metadata.logical_size = node.symlink_target->size();
      node.metadata.allocated_size = node.symlink_target->size();
      if (node.metadata.link_count == 0U) {
        node.metadata.link_count = 1U;
      }
    }
  } else {
    node.metadata.kind = orchard::apfs::InodeKind::kRegularFile;
    node.metadata.logical_size = node.symlink_target->size();
    node.metadata.allocated_size = node.symlink_target->size();
    if (node.metadata.link_count == 0U) {
      node.metadata.link_count = 1U;
    }
  }

  node.symlink_reparse_eligible = false;
  node.metadata_complete = true;
  return true;
}

blockio::Result<orchard::apfs::FileMetadata>
MountedVolume::ResolveReadableMetadata(const FileNode& node) const {
  if (node.metadata_complete || node.metadata.kind != orchard::apfs::InodeKind::kRegularFile) {
    auto metadata = node.metadata;
    if (!node.metadata_complete) {
      auto metadata_result = orchard::apfs::GetFileMetadata(context_, node.inode_id);
      if (!metadata_result.ok()) {
        return metadata_result.error();
      }
      metadata = metadata_result.value();
    }
    return metadata;
  }

  orchard::apfs::FileMetadata metadata = node.metadata;
  orchard::apfs::CompressionInfo compression_info;
  bool compression_info_cached = false;
  {
    std::scoped_lock lock(read_cache_mutex_);
    const auto existing = read_cache_by_inode_.find(node.inode_id);
    if (existing != read_cache_by_inode_.end() && existing->second.compression_info_cached) {
      ++compression_info_cache_hits_;
      compression_info = existing->second.compression_info;
      compression_info_cached = true;
    }
  }

  if (!compression_info_cached) {
    ++compression_info_cache_misses_;
    auto compression_xattr_result =
        context_.FindXattr(node.inode_id, orchard::apfs::kCompressionXattrName);
    if (!compression_xattr_result.ok()) {
      return compression_xattr_result.error();
    }
    if (compression_xattr_result.value().has_value()) {
      const auto& compression_data = compression_xattr_result.value()->data;
      auto compression_result = orchard::apfs::ParseCompressionInfo(
          std::span<const std::uint8_t>(compression_data.data(), compression_data.size()));
      if (!compression_result.ok()) {
        return compression_result.error();
      }
      compression_info = compression_result.value();
    }

    {
      std::scoped_lock lock(read_cache_mutex_);
      auto& cache_entry = read_cache_by_inode_[node.inode_id];
      cache_entry.compression_info = compression_info;
      cache_entry.compression_info_cached = true;
    }
  }

  metadata.compression = compression_info;
  if (compression_info.uncompressed_size != 0U) {
    metadata.logical_size = compression_info.uncompressed_size;
  }
  return metadata;
}

blockio::Result<FileNode> MountedVolume::ResolveFileNode(std::string_view normalized_path) const {
  ++resolve_file_node_calls_;
  std::optional<FileNode> cached_node;
  {
    std::scoped_lock lock(nodes_mutex_);
    const auto existing = nodes_by_path_.find(std::string(normalized_path));
    if (existing != nodes_by_path_.end() && existing->second.node) {
      ++resolve_file_node_cache_hits_;
      cached_node = *existing->second.node;
    }
  }
  if (cached_node.has_value()) {
    auto projection_result = ProjectSymlinkNode(*cached_node);
    if (!projection_result.ok()) {
      return projection_result.error();
    }
    return *CacheResolvedNode(std::move(*cached_node));
  }

  auto lookup_result = orchard::apfs::LookupPath(context_, normalized_path);
  if (!lookup_result.ok()) {
    return lookup_result.error();
  }

  auto metadata_result =
      orchard::apfs::GetFileMetadata(context_, lookup_result.value().inode.key.header.object_id);
  if (!metadata_result.ok()) {
    return metadata_result.error();
  }

  FileNode node;
  node.inode_id = lookup_result.value().inode.key.header.object_id;
  node.parent_inode_id = lookup_result.value().inode.parent_id;
  node.normalized_path = lookup_result.value().normalized_path;
  node.metadata = metadata_result.value();
  node.metadata_complete = true;
  auto projection_result = ProjectSymlinkNode(node);
  if (!projection_result.ok()) {
    return projection_result.error();
  }

  {
    return *CacheResolvedNode(std::move(node));
  }
}

blockio::Result<std::vector<orchard::apfs::DirectoryEntryRecord>>
MountedVolume::ListDirectoryEntries(const std::uint64_t directory_inode_id) const {
  auto entries_view_result = GetDirectoryEntriesView(directory_inode_id);
  if (!entries_view_result.ok()) {
    return entries_view_result.error();
  }
  return *entries_view_result.value();
}

blockio::Result<DirectoryEntryListHandle>
MountedVolume::GetDirectoryEntriesView(const std::uint64_t directory_inode_id) const {
  {
    std::scoped_lock lock(directory_entries_mutex_);
    const auto existing = directory_entries_by_inode_.find(directory_inode_id);
    if (existing != directory_entries_by_inode_.end()) {
      ++directory_cache_hits_;
      return existing->second;
    }
  }

  ++directory_cache_misses_;
  auto entries_result = orchard::apfs::ListDirectory(context_, directory_inode_id);
  if (!entries_result.ok()) {
    return entries_result.error();
  }
  auto cached_entries = std::make_shared<const std::vector<orchard::apfs::DirectoryEntryRecord>>(
      std::move(entries_result.value()));

  {
    std::scoped_lock lock(directory_entries_mutex_);
    auto [entry, inserted] =
        directory_entries_by_inode_.try_emplace(directory_inode_id, cached_entries);
    if (inserted) {
      return entry->second;
    }
    return entry->second;
  }
}

blockio::Result<std::vector<std::uint8_t>>
MountedVolume::ReadFileRange(const orchard::apfs::FileReadRequest& request) const {
  return orchard::apfs::ReadFileRange(context_, request);
}

blockio::Result<std::vector<std::uint8_t>>
MountedVolume::ReadFileRange(const FileNode& node,
                             const orchard::apfs::FileReadRequest& request) const {
  auto metadata_result = ResolveReadableMetadata(node);
  if (!metadata_result.ok()) {
    return metadata_result.error();
  }
  auto metadata = metadata_result.value();

  if (!orchard::apfs::IsRegularFile(metadata.kind) &&
      metadata.kind != orchard::apfs::InodeKind::kSymlink) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Requested inode is not readable as a file.");
  }
  if (request.size == 0U || request.offset >= metadata.logical_size) {
    return std::vector<std::uint8_t>{};
  }

  const auto remaining = metadata.logical_size - request.offset;
  const auto bounded_size = static_cast<std::size_t>(
      std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(request.size)));

  if (metadata.compression.kind != orchard::apfs::CompressionKind::kNone) {
    std::shared_ptr<const std::vector<std::uint8_t>> decoded_payload;
    {
      std::scoped_lock lock(read_cache_mutex_);
      const auto existing = read_cache_by_inode_.find(node.inode_id);
      if (existing != read_cache_by_inode_.end() && existing->second.decoded_compression_payload) {
        ++compression_cache_hits_;
        decoded_payload = existing->second.decoded_compression_payload;
      }
    }

    if (!decoded_payload) {
      ++compression_cache_misses_;
      auto compression_xattr_result =
          context_.FindXattr(node.inode_id, orchard::apfs::kCompressionXattrName);
      if (!compression_xattr_result.ok()) {
        return compression_xattr_result.error();
      }
      if (!compression_xattr_result.value().has_value()) {
        return orchard::apfs::MakeApfsError(
            blockio::ErrorCode::kNotFound,
            "Compression metadata was not found for a compressed APFS file.");
      }

      const auto& compression_data = compression_xattr_result.value()->data;
      auto decoded_result = orchard::apfs::DecodeCompressionPayload(
          std::span<const std::uint8_t>(compression_data.data(), compression_data.size()));
      if (!decoded_result.ok()) {
        return decoded_result.error();
      }

      auto decoded_bytes =
          std::make_shared<const std::vector<std::uint8_t>>(std::move(decoded_result.value()));
      read_fetched_bytes_.fetch_add(decoded_bytes->size());
      {
        std::scoped_lock lock(read_cache_mutex_);
        auto& cache_entry = read_cache_by_inode_[node.inode_id];
        if (!cache_entry.decoded_compression_payload) {
          cache_entry.decoded_compression_payload = decoded_bytes;
        }
        decoded_payload = cache_entry.decoded_compression_payload;
      }
    }

    return std::vector<std::uint8_t>(
        decoded_payload->begin() + static_cast<std::ptrdiff_t>(request.offset),
        decoded_payload->begin() + static_cast<std::ptrdiff_t>(request.offset + bounded_size));
  }

  std::shared_ptr<const std::vector<orchard::apfs::FileExtentRecord>> extents;
  std::shared_ptr<const std::vector<std::uint8_t>> cached_read_ahead_bytes;
  std::uint64_t cached_read_ahead_offset = 0U;
  bool should_prefetch = false;
  std::size_t read_ahead_size = 0U;
  {
    std::scoped_lock lock(read_cache_mutex_);
    auto existing = read_cache_by_inode_.find(node.inode_id);
    if (existing != read_cache_by_inode_.end() && existing->second.extents) {
      ++read_extent_cache_hits_;
      extents = existing->second.extents;
    }
    if (existing != read_cache_by_inode_.end() && existing->second.read_ahead_bytes) {
      const auto cached_range_start = existing->second.read_ahead_offset;
      const auto cached_range_end = cached_range_start + existing->second.read_ahead_bytes->size();
      const auto request_end = request.offset + bounded_size;
      if (request.offset >= cached_range_start && request_end <= cached_range_end) {
        ++read_ahead_hits_;
        cached_read_ahead_bytes = existing->second.read_ahead_bytes;
        cached_read_ahead_offset = existing->second.read_ahead_offset;
      }
    }

    if (cached_read_ahead_bytes) {
      // The cached range fully covers this request; skip sequential state updates.
    } else {
      auto& cache_entry = read_cache_by_inode_[node.inode_id];
      const auto expected_offset = cache_entry.last_request_offset + cache_entry.last_request_size;
      if (cache_entry.last_request_size != 0U && request.offset == expected_offset) {
        ++cache_entry.sequential_read_count;
      } else {
        cache_entry.sequential_read_count = 1U;
        cache_entry.read_ahead_bytes.reset();
        cache_entry.read_ahead_offset = 0U;
      }
      cache_entry.last_request_offset = request.offset;
      cache_entry.last_request_size = bounded_size;

      const bool allow_small_request_read_ahead = metadata.logical_size >= kReadAheadLargeFileBytes;
      if (cache_entry.sequential_read_count >= kReadAheadTriggerSequentialReads &&
          metadata.logical_size >= kReadAheadMinFileBytes &&
          (bounded_size >= kReadAheadMinRequestBytes || allow_small_request_read_ahead)) {
        std::size_t desired_window = std::max<std::size_t>(
            bounded_size,
            allow_small_request_read_ahead ? kReadAheadLargeFileMinBytes : kReadAheadMinBytes);
        if (bounded_size >= kReadAheadMinRequestBytes &&
            bounded_size <= (kReadAheadMaxBytes / kReadAheadGrowthFactor)) {
          desired_window =
              std::max<std::size_t>(desired_window, bounded_size * kReadAheadGrowthFactor);
        }
        desired_window = std::max<std::size_t>(desired_window, bounded_size);
        desired_window = std::min<std::size_t>(desired_window, kReadAheadMaxBytes);
        const auto remaining_window = static_cast<std::size_t>(std::min<std::uint64_t>(
            metadata.logical_size - request.offset, static_cast<std::uint64_t>(desired_window)));
        if (remaining_window > bounded_size) {
          should_prefetch = true;
          read_ahead_size = remaining_window;
        }
      }
    }
  }

  if (cached_read_ahead_bytes) {
    const auto start = static_cast<std::size_t>(request.offset - cached_read_ahead_offset);
    return std::vector<std::uint8_t>(
        cached_read_ahead_bytes->begin() + static_cast<std::ptrdiff_t>(start),
        cached_read_ahead_bytes->begin() + static_cast<std::ptrdiff_t>(start + bounded_size));
  }

  if (!extents) {
    ++read_extent_cache_misses_;
    auto extents_result = context_.ListFileExtents(node.inode_id);
    if (!extents_result.ok()) {
      return extents_result.error();
    }

    auto cached_extents = std::make_shared<const std::vector<orchard::apfs::FileExtentRecord>>(
        std::move(extents_result.value()));
    {
      std::scoped_lock lock(read_cache_mutex_);
      auto& cache_entry = read_cache_by_inode_[node.inode_id];
      if (!cache_entry.extents) {
        cache_entry.extents = cached_extents;
      }
      extents = cache_entry.extents;
    }
  }
  if (should_prefetch) {
    auto prefetched_bytes_result =
        ReadRegularFileBytes(context_, *extents, request.offset, read_ahead_size);
    if (!prefetched_bytes_result.ok()) {
      return prefetched_bytes_result.error();
    }

    auto prefetched_bytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::move(prefetched_bytes_result.value()));
    read_fetched_bytes_.fetch_add(prefetched_bytes->size());
    {
      std::scoped_lock lock(read_cache_mutex_);
      auto& cache_entry = read_cache_by_inode_[node.inode_id];
      cache_entry.read_ahead_bytes = prefetched_bytes;
      cache_entry.read_ahead_offset = request.offset;
      ++read_ahead_prefetches_;
    }

    return std::vector<std::uint8_t>(prefetched_bytes->begin(),
                                     prefetched_bytes->begin() +
                                         static_cast<std::ptrdiff_t>(bounded_size));
  }

  read_fetched_bytes_.fetch_add(bounded_size);
  return ReadRegularFileBytes(context_, *extents, request.offset, bounded_size);
}

blockio::Result<std::string> MountedVolume::ReadSymlinkTarget(const std::uint64_t inode_id) const {
  return orchard::apfs::ReadSymlinkTarget(context_, inode_id);
}

void MountedVolume::PrimeDirectoryChildren(
    const FileNode& directory_node,
    const std::span<const orchard::apfs::DirectoryEntryRecord> entries) const {
  if (directory_node.metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return;
  }

  for (const auto& entry : entries) {
    if (!entry.inode.has_value()) {
      continue;
    }

    FileNode child_node;
    child_node.inode_id = entry.file_id;
    child_node.parent_inode_id = directory_node.inode_id;
    child_node.normalized_path = ComposeChildPath(directory_node.normalized_path, entry.key.name);
    child_node.metadata = MakeMetadataFromInode(*entry.inode);
    child_node.metadata_complete = false;
    static_cast<void>(CacheResolvedNode(std::move(child_node)));
  }
}

void MountedVolume::RecordObservedRead(const std::uint64_t file_logical_size,
                                       const std::size_t served_size,
                                       const bool sequential_request) const noexcept {
  read_requested_bytes_.fetch_add(served_size);
  if (served_size <= kSmallReadRequestBytes) {
    ++read_requests_small_;
  } else if (served_size <= kReadAheadMinRequestBytes) {
    ++read_requests_medium_;
  } else {
    ++read_requests_large_;
  }

  if (sequential_request) {
    ++read_requests_sequential_;
  } else {
    ++read_requests_non_sequential_;
  }

  if (file_logical_size >= kReadAheadMinFileBytes && served_size < kReadAheadMinRequestBytes) {
    ++large_file_small_request_reads_;
    if (sequential_request) {
      ++large_file_small_request_sequential_reads_;
    }
  }
}

void MountedVolume::RecordCompletedOpenReadCount(
    const std::uint64_t read_call_count) const noexcept {
  if (read_call_count == 0U) {
    ++open_handles_zero_reads_;
  } else if (read_call_count == 1U) {
    ++open_handles_one_read_;
  } else if (read_call_count <= 4U) {
    ++open_handles_two_to_four_reads_;
  } else {
    ++open_handles_five_plus_reads_;
  }
}

blockio::Result<std::shared_ptr<FileNode>>
MountedVolume::CreateOpenNode(std::string_view normalized_path) {
  auto resolved_result = ResolveFileNode(normalized_path);
  if (!resolved_result.ok()) {
    return resolved_result.error();
  }

  std::scoped_lock lock(nodes_mutex_);
  const auto existing = nodes_by_path_.find(resolved_result.value().normalized_path);
  if (existing != nodes_by_path_.end() && existing->second.node) {
    return existing->second.node;
  }
  return std::make_shared<FileNode>(std::move(resolved_result.value()));
}

std::shared_ptr<FileNode> MountedVolume::CacheResolvedNode(FileNode node) const {
  std::scoped_lock lock(nodes_mutex_);
  auto [entry, inserted] = nodes_by_path_.try_emplace(node.normalized_path, NodeCacheEntry{});
  if (inserted || !entry->second.node) {
    entry->second.node = std::make_shared<FileNode>(std::move(node));
    return entry->second.node;
  }

  if (!entry->second.node->metadata_complete && node.metadata_complete) {
    entry->second.node->metadata = std::move(node.metadata);
    entry->second.node->metadata_complete = true;
    entry->second.node->parent_inode_id = node.parent_inode_id;
  }
  if (!entry->second.node->symlink_target.has_value() && node.symlink_target.has_value()) {
    entry->second.node->symlink_target = std::move(node.symlink_target);
  }
  entry->second.node->symlink_reparse_eligible = node.symlink_reparse_eligible;
  return entry->second.node;
}

blockio::Result<std::shared_ptr<FileNode>>
MountedVolume::AcquireOpenNode(std::string_view normalized_path) {
  std::shared_ptr<FileNode> cached_node;
  std::string cached_path;
  bool needs_refresh = false;
  {
    std::scoped_lock lock(nodes_mutex_);
    const auto existing = nodes_by_path_.find(std::string(normalized_path));
    if (existing != nodes_by_path_.end() && existing->second.node) {
      ++existing->second.open_handle_count;
      path_by_node_[existing->second.node.get()] = existing->first;
      cached_node = existing->second.node;
      cached_path = existing->first;
      needs_refresh = cached_node->metadata.kind == orchard::apfs::InodeKind::kSymlink &&
                      !cached_node->symlink_target.has_value();
    }
  }

  if (cached_node) {
    if (needs_refresh) {
      auto refreshed_result = ResolveFileNode(cached_path);
      if (!refreshed_result.ok()) {
        ReleaseOpenNode(cached_node.get());
        return refreshed_result.error();
      }
    }
    return cached_node;
  }

  auto created_result = CreateOpenNode(normalized_path);
  if (!created_result.ok()) {
    return created_result.error();
  }

  const auto canonical_path = created_result.value()->normalized_path;
  std::scoped_lock lock(nodes_mutex_);
  auto [entry, inserted] = nodes_by_path_.try_emplace(canonical_path, NodeCacheEntry{});
  if (inserted) {
    entry->second.node = created_result.value();
    entry->second.open_handle_count = 1U;
    path_by_node_[entry->second.node.get()] = entry->first;
    return entry->second.node;
  }

  ++entry->second.open_handle_count;
  path_by_node_[entry->second.node.get()] = entry->first;
  return entry->second.node;
}

blockio::Result<std::shared_ptr<FileNode>>
MountedVolume::GetOpenNode(const FileNode* raw_node) const {
  std::scoped_lock lock(nodes_mutex_);
  const auto path_it = path_by_node_.find(raw_node);
  if (path_it == path_by_node_.end()) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kNotFound,
                                        "The WinFsp file context does not map to an open node.");
  }
  const auto node_it = nodes_by_path_.find(path_it->second);
  if (node_it == nodes_by_path_.end() || !node_it->second.node) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kNotFound,
                                        "The WinFsp node cache entry is missing.");
  }

  return node_it->second.node;
}

void MountedVolume::ReleaseOpenNode(const FileNode* raw_node) {
  std::scoped_lock lock(nodes_mutex_);
  const auto path_it = path_by_node_.find(raw_node);
  if (path_it == path_by_node_.end()) {
    return;
  }

  const auto node_it = nodes_by_path_.find(path_it->second);
  if (node_it == nodes_by_path_.end()) {
    path_by_node_.erase(path_it);
    return;
  }

  if (node_it->second.open_handle_count > 1U) {
    --node_it->second.open_handle_count;
    return;
  }

  path_by_node_.erase(path_it);
  if (node_it->second.node) {
    const auto inode_id = node_it->second.node->inode_id;
    std::scoped_lock read_lock(read_cache_mutex_);
    const auto cache_it = read_cache_by_inode_.find(inode_id);
    if (cache_it != read_cache_by_inode_.end()) {
      cache_it->second.read_ahead_bytes.reset();
      cache_it->second.read_ahead_offset = 0U;
      cache_it->second.last_request_offset = 0U;
      cache_it->second.last_request_size = 0U;
      cache_it->second.sequential_read_count = 0U;
    }
  }
  node_it->second.open_handle_count = 0U;
}

MountedVolumePerformanceStats MountedVolume::performance_stats() const noexcept {
  MountedVolumePerformanceStats stats;
  stats.resolve_file_node_calls = resolve_file_node_calls_.load();
  stats.resolve_file_node_cache_hits = resolve_file_node_cache_hits_.load();
  stats.directory_cache_hits = directory_cache_hits_.load();
  stats.directory_cache_misses = directory_cache_misses_.load();
  stats.symlink_target_loads = symlink_target_loads_.load();
  stats.read_extent_cache_hits = read_extent_cache_hits_.load();
  stats.read_extent_cache_misses = read_extent_cache_misses_.load();
  stats.read_ahead_hits = read_ahead_hits_.load();
  stats.read_ahead_prefetches = read_ahead_prefetches_.load();
  stats.compression_info_cache_hits = compression_info_cache_hits_.load();
  stats.compression_info_cache_misses = compression_info_cache_misses_.load();
  stats.compression_cache_hits = compression_cache_hits_.load();
  stats.compression_cache_misses = compression_cache_misses_.load();
  stats.read_requested_bytes = read_requested_bytes_.load();
  stats.read_fetched_bytes = read_fetched_bytes_.load();
  stats.read_requests_small = read_requests_small_.load();
  stats.read_requests_medium = read_requests_medium_.load();
  stats.read_requests_large = read_requests_large_.load();
  stats.read_requests_sequential = read_requests_sequential_.load();
  stats.read_requests_non_sequential = read_requests_non_sequential_.load();
  stats.large_file_small_request_reads = large_file_small_request_reads_.load();
  stats.large_file_small_request_sequential_reads =
      large_file_small_request_sequential_reads_.load();
  stats.open_handles_zero_reads = open_handles_zero_reads_.load();
  stats.open_handles_one_read = open_handles_one_read_.load();
  stats.open_handles_two_to_four_reads = open_handles_two_to_four_reads_.load();
  stats.open_handles_five_plus_reads = open_handles_five_plus_reads_.load();
  return stats;
}

blockio::Result<MountedVolumeHandle> OpenMountedVolume(const MountConfig& config) {
  return MountedVolume::Open(config);
}

blockio::Result<MountSessionHandle> StartMount(const MountConfig& config) {
  auto mounted_volume_result = OpenMountedVolume(config);
  if (!mounted_volume_result.ok()) {
    return mounted_volume_result.error();
  }

  return CreateFileSystemSession(config, mounted_volume_result.value());
}

bool HasWinFspSupport() noexcept {
  return IsWinFspBackendAvailable();
}

std::string_view WinFspSupportStatus() noexcept {
  return WinFspBackendStatus();
}

std::string_view ToString(const MountCallbackId callback) noexcept {
  switch (callback) {
  case MountCallbackId::kGetSecurityByName:
    return "GetSecurityByName";
  case MountCallbackId::kCreate:
    return "Create";
  case MountCallbackId::kOpen:
    return "Open";
  case MountCallbackId::kGetFileInfo:
    return "GetFileInfo";
  case MountCallbackId::kGetDirInfoByName:
    return "GetDirInfoByName";
  case MountCallbackId::kReadDirectory:
    return "ReadDirectory";
  case MountCallbackId::kRead:
    return "Read";
  case MountCallbackId::kResolveReparsePoints:
    return "ResolveReparsePoints";
  case MountCallbackId::kGetReparsePoint:
    return "GetReparsePoint";
  case MountCallbackId::kClose:
    return "Close";
  case MountCallbackId::kGetSecurity:
    return "GetSecurity";
  case MountCallbackId::kGetStreamInfo:
    return "GetStreamInfo";
  case MountCallbackId::kCount:
    break;
  }

  return "Unknown";
}

} // namespace orchard::fs_winfsp
