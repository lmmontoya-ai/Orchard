#include "orchard/fs_winfsp/mount.h"

#include <Windows.h>
#include <sddl.h>

#include <cstring>
#include <sstream>
#include <utility>

#include "orchard/apfs/omap.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/fs_winfsp/filesystem.h"
#include "orchard/fs_winfsp/path_bridge.h"

namespace orchard::fs_winfsp {
namespace {

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
        return orchard::apfs::MakeApfsError(blockio::ErrorCode::kUnsupportedTarget,
                                            message.str());
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

} // namespace

MountedVolume::MountedVolume(MountConfig config, blockio::InspectionTargetInfo target_info,
                             blockio::ReaderHandle reader, orchard::apfs::ContainerInfo container,
                             orchard::apfs::VolumeInfo volume,
                             orchard::apfs::VolumeContext context,
                             std::vector<std::uint8_t> security_descriptor,
                             std::wstring volume_label,
                             const std::uint32_t volume_serial_number)
    : config_(std::move(config)), target_info_(std::move(target_info)), reader_(std::move(reader)),
      container_(std::move(container)), volume_(std::move(volume)), context_(std::move(context)),
      security_descriptor_(std::move(security_descriptor)),
      volume_label_(std::move(volume_label)), volume_serial_number_(volume_serial_number) {}

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
  const auto& selected_volume =
      selected_container.volumes[selected_result.value().volume_index];

  orchard::apfs::PhysicalObjectReader object_reader(*reader, selected_container.byte_offset,
                                                    selected_container.block_size);
  auto container_omap_result =
      orchard::apfs::OmapResolver::Load(object_reader, selected_container.omap_oid);
  if (!container_omap_result.ok()) {
    return container_omap_result.error();
  }

  auto context_result =
      orchard::apfs::VolumeContext::Load(*reader, selected_container, selected_volume,
                                         container_omap_result.value());
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

blockio::Result<FileNode> MountedVolume::ResolveFileNode(std::string_view normalized_path) const {
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
  return node;
}

blockio::Result<std::vector<orchard::apfs::DirectoryEntryRecord>>
MountedVolume::ListDirectoryEntries(const std::uint64_t directory_inode_id) const {
  return orchard::apfs::ListDirectory(context_, directory_inode_id);
}

blockio::Result<std::vector<std::uint8_t>>
MountedVolume::ReadFileRange(const orchard::apfs::FileReadRequest& request) const {
  return orchard::apfs::ReadFileRange(context_, request);
}

blockio::Result<std::shared_ptr<FileNode>>
MountedVolume::CreateOpenNode(std::string_view normalized_path) {
  auto node_result = ResolveFileNode(normalized_path);
  if (!node_result.ok()) {
    return node_result.error();
  }
  return std::make_shared<FileNode>(std::move(node_result.value()));
}

blockio::Result<std::shared_ptr<FileNode>>
MountedVolume::AcquireOpenNode(std::string_view normalized_path) {
  {
    std::scoped_lock lock(nodes_mutex_);
    const auto existing = nodes_by_path_.find(std::string(normalized_path));
    if (existing != nodes_by_path_.end()) {
      ++existing->second.open_handle_count;
      return existing->second.node;
    }
  }

  auto created_result = CreateOpenNode(normalized_path);
  if (!created_result.ok()) {
    return created_result.error();
  }

  std::scoped_lock lock(nodes_mutex_);
  auto [entry, inserted] =
      nodes_by_path_.try_emplace(std::string(normalized_path), NodeCacheEntry{});
  if (inserted) {
    entry->second.node = created_result.value();
    entry->second.open_handle_count = 1U;
    path_by_node_[entry->second.node.get()] = entry->first;
    return entry->second.node;
  }

  ++entry->second.open_handle_count;
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
  nodes_by_path_.erase(node_it);
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

} // namespace orchard::fs_winfsp
