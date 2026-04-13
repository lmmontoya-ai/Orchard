#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "orchard/apfs/discovery.h"
#include "orchard/apfs/format.h"
#include "orchard/apfs/volume.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/reader.h"
#include "orchard/fs_winfsp/types.h"

namespace orchard::fs_winfsp {

class MountedVolume;
class MountSession;

using MountedVolumeHandle = std::shared_ptr<MountedVolume>;
using MountSessionHandle = std::unique_ptr<MountSession>;

class MountedVolume {
public:
  static blockio::Result<MountedVolumeHandle> Open(const MountConfig& config);

  [[nodiscard]] const MountConfig& config() const noexcept {
    return config_;
  }
  [[nodiscard]] const orchard::apfs::ContainerInfo& container_info() const noexcept {
    return container_;
  }
  [[nodiscard]] const orchard::apfs::VolumeInfo& volume_info() const noexcept {
    return volume_;
  }
  [[nodiscard]] const orchard::apfs::VolumeContext& volume_context() const noexcept {
    return context_;
  }
  [[nodiscard]] const std::vector<std::uint8_t>& security_descriptor() const noexcept {
    return security_descriptor_;
  }
  [[nodiscard]] std::wstring_view volume_label() const noexcept {
    return volume_label_;
  }
  [[nodiscard]] std::uint32_t volume_serial_number() const noexcept {
    return volume_serial_number_;
  }

  [[nodiscard]] blockio::Result<FileNode> ResolveFileNode(std::string_view normalized_path) const;
  [[nodiscard]] blockio::Result<std::vector<orchard::apfs::DirectoryEntryRecord>>
  ListDirectoryEntries(std::uint64_t directory_inode_id) const;
  [[nodiscard]] blockio::Result<std::vector<std::uint8_t>>
  ReadFileRange(const orchard::apfs::FileReadRequest& request) const;

  [[nodiscard]] blockio::Result<std::shared_ptr<FileNode>>
  AcquireOpenNode(std::string_view normalized_path);
  [[nodiscard]] blockio::Result<std::shared_ptr<FileNode>>
  GetOpenNode(const FileNode* raw_node) const;
  void ReleaseOpenNode(const FileNode* raw_node);

private:
  struct NodeCacheEntry {
    std::shared_ptr<FileNode> node;
    std::size_t open_handle_count = 0;
  };

  MountedVolume(MountConfig config, blockio::InspectionTargetInfo target_info,
                blockio::ReaderHandle reader, orchard::apfs::ContainerInfo container,
                orchard::apfs::VolumeInfo volume, orchard::apfs::VolumeContext context,
                std::vector<std::uint8_t> security_descriptor, std::wstring volume_label,
                std::uint32_t volume_serial_number);

  [[nodiscard]] blockio::Result<std::shared_ptr<FileNode>>
  CreateOpenNode(std::string_view normalized_path);

  MountConfig config_;
  blockio::InspectionTargetInfo target_info_;
  blockio::ReaderHandle reader_;
  orchard::apfs::ContainerInfo container_;
  orchard::apfs::VolumeInfo volume_;
  orchard::apfs::VolumeContext context_;
  std::vector<std::uint8_t> security_descriptor_;
  std::wstring volume_label_;
  std::uint32_t volume_serial_number_ = 0;

  mutable std::mutex nodes_mutex_;
  std::unordered_map<std::string, NodeCacheEntry> nodes_by_path_;
  std::unordered_map<const FileNode*, std::string> path_by_node_;
};

class MountSession {
public:
  virtual ~MountSession() = default;

  [[nodiscard]] virtual MountedVolume& mounted_volume() noexcept = 0;
  [[nodiscard]] virtual const MountedVolume& mounted_volume() const noexcept = 0;
  [[nodiscard]] virtual std::wstring_view mount_point() const noexcept = 0;
  virtual void Stop() noexcept = 0;
};

blockio::Result<MountedVolumeHandle> OpenMountedVolume(const MountConfig& config);
blockio::Result<MountSessionHandle> StartMount(const MountConfig& config);
bool HasWinFspSupport() noexcept;
std::string_view WinFspSupportStatus() noexcept;

} // namespace orchard::fs_winfsp
