#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "orchard/apfs/discovery.h"
#include "orchard/apfs/format.h"
#include "orchard/apfs/link_read.h"
#include "orchard/apfs/volume.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/reader.h"
#include "orchard/fs_winfsp/types.h"

namespace orchard::fs_winfsp {

class MountedVolume;
class MountSession;

using MountedVolumeHandle = std::shared_ptr<MountedVolume>;
using MountSessionHandle = std::unique_ptr<MountSession>;
using DirectoryEntryListHandle =
    std::shared_ptr<const std::vector<orchard::apfs::DirectoryEntryRecord>>;

enum class MountCallbackId : std::uint8_t {
  kGetSecurityByName = 0,
  kCreate,
  kOpen,
  kGetFileInfo,
  kGetDirInfoByName,
  kReadDirectory,
  kRead,
  kResolveReparsePoints,
  kGetReparsePoint,
  kClose,
  kGetSecurity,
  kGetStreamInfo,
  kCount,
};

constexpr std::size_t kMountCallbackCount = static_cast<std::size_t>(MountCallbackId::kCount);

struct CallbackPerformanceStats {
  std::uint64_t call_count = 0;
  std::uint64_t total_microseconds = 0;
  std::uint64_t max_microseconds = 0;
  std::uint64_t slow_over_50ms = 0;
  std::uint64_t slow_over_200ms = 0;
};

struct MountSessionPerformanceStats {
  std::array<CallbackPerformanceStats, kMountCallbackCount> callbacks{};
};

struct MountedVolumePerformanceStats {
  std::uint64_t resolve_file_node_calls = 0;
  std::uint64_t resolve_file_node_cache_hits = 0;
  std::uint64_t directory_cache_hits = 0;
  std::uint64_t directory_cache_misses = 0;
  std::uint64_t symlink_target_loads = 0;
  std::uint64_t read_extent_cache_hits = 0;
  std::uint64_t read_extent_cache_misses = 0;
  std::uint64_t read_ahead_hits = 0;
  std::uint64_t read_ahead_prefetches = 0;
  std::uint64_t compression_info_cache_hits = 0;
  std::uint64_t compression_info_cache_misses = 0;
  std::uint64_t compression_cache_hits = 0;
  std::uint64_t compression_cache_misses = 0;
  std::uint64_t read_requested_bytes = 0;
  std::uint64_t read_fetched_bytes = 0;
  std::uint64_t read_requests_small = 0;
  std::uint64_t read_requests_medium = 0;
  std::uint64_t read_requests_large = 0;
  std::uint64_t read_requests_sequential = 0;
  std::uint64_t read_requests_non_sequential = 0;
  std::uint64_t large_file_small_request_reads = 0;
  std::uint64_t large_file_small_request_sequential_reads = 0;
  std::uint64_t open_handles_zero_reads = 0;
  std::uint64_t open_handles_one_read = 0;
  std::uint64_t open_handles_two_to_four_reads = 0;
  std::uint64_t open_handles_five_plus_reads = 0;
};

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
  [[nodiscard]] blockio::Result<DirectoryEntryListHandle>
  GetDirectoryEntriesView(std::uint64_t directory_inode_id) const;
  [[nodiscard]] blockio::Result<std::vector<orchard::apfs::DirectoryEntryRecord>>
  ListDirectoryEntries(std::uint64_t directory_inode_id) const;
  [[nodiscard]] blockio::Result<std::vector<std::uint8_t>>
  ReadFileRange(const orchard::apfs::FileReadRequest& request) const;
  [[nodiscard]] blockio::Result<std::vector<std::uint8_t>>
  ReadFileRange(const FileNode& node, const orchard::apfs::FileReadRequest& request) const;
  [[nodiscard]] blockio::Result<std::string> ReadSymlinkTarget(std::uint64_t inode_id) const;
  void PrimeDirectoryChildren(const FileNode& directory_node,
                              std::span<const orchard::apfs::DirectoryEntryRecord> entries) const;
  void RecordObservedRead(std::uint64_t file_logical_size, std::size_t served_size,
                          bool sequential_request) const noexcept;
  void RecordCompletedOpenReadCount(std::uint64_t read_call_count) const noexcept;
  [[nodiscard]] MountedVolumePerformanceStats performance_stats() const noexcept;

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

  struct ReadCacheEntry {
    std::shared_ptr<const std::vector<orchard::apfs::FileExtentRecord>> extents;
    std::shared_ptr<const std::vector<std::uint8_t>> decoded_compression_payload;
    std::shared_ptr<const std::vector<std::uint8_t>> read_ahead_bytes;
    orchard::apfs::CompressionInfo compression_info;
    bool compression_info_cached = false;
    std::uint64_t read_ahead_offset = 0;
    std::uint64_t last_request_offset = 0;
    std::size_t last_request_size = 0;
    std::uint32_t sequential_read_count = 0;
  };

  MountedVolume(MountConfig config, blockio::InspectionTargetInfo target_info,
                blockio::ReaderHandle reader, orchard::apfs::ContainerInfo container,
                orchard::apfs::VolumeInfo volume, orchard::apfs::VolumeContext context,
                std::vector<std::uint8_t> security_descriptor, std::wstring volume_label,
                std::uint32_t volume_serial_number);

  [[nodiscard]] blockio::Result<bool> ProjectSymlinkNode(FileNode& node) const;
  [[nodiscard]] blockio::Result<orchard::apfs::FileMetadata>
  ResolveReadableMetadata(const FileNode& node) const;
  [[nodiscard]] blockio::Result<std::shared_ptr<FileNode>>
  CreateOpenNode(std::string_view normalized_path);
  [[nodiscard]] std::shared_ptr<FileNode> CacheResolvedNode(FileNode node) const;

  MountConfig config_;
  blockio::InspectionTargetInfo target_info_;
  blockio::ReaderHandle reader_;
  orchard::apfs::ContainerInfo container_;
  orchard::apfs::VolumeInfo volume_;
  orchard::apfs::VolumeContext context_;
  std::vector<std::uint8_t> security_descriptor_;
  std::wstring volume_label_;
  std::uint32_t volume_serial_number_ = 0;

  mutable std::mutex directory_entries_mutex_;
  mutable std::unordered_map<std::uint64_t, DirectoryEntryListHandle> directory_entries_by_inode_;

  mutable std::mutex nodes_mutex_;
  mutable std::unordered_map<std::string, NodeCacheEntry> nodes_by_path_;
  std::unordered_map<const FileNode*, std::string> path_by_node_;

  mutable std::mutex read_cache_mutex_;
  mutable std::unordered_map<std::uint64_t, ReadCacheEntry> read_cache_by_inode_;

  mutable std::atomic<std::uint64_t> resolve_file_node_calls_{0U};
  mutable std::atomic<std::uint64_t> resolve_file_node_cache_hits_{0U};
  mutable std::atomic<std::uint64_t> directory_cache_hits_{0U};
  mutable std::atomic<std::uint64_t> directory_cache_misses_{0U};
  mutable std::atomic<std::uint64_t> symlink_target_loads_{0U};
  mutable std::atomic<std::uint64_t> read_extent_cache_hits_{0U};
  mutable std::atomic<std::uint64_t> read_extent_cache_misses_{0U};
  mutable std::atomic<std::uint64_t> read_ahead_hits_{0U};
  mutable std::atomic<std::uint64_t> read_ahead_prefetches_{0U};
  mutable std::atomic<std::uint64_t> compression_info_cache_hits_{0U};
  mutable std::atomic<std::uint64_t> compression_info_cache_misses_{0U};
  mutable std::atomic<std::uint64_t> compression_cache_hits_{0U};
  mutable std::atomic<std::uint64_t> compression_cache_misses_{0U};
  mutable std::atomic<std::uint64_t> read_requested_bytes_{0U};
  mutable std::atomic<std::uint64_t> read_fetched_bytes_{0U};
  mutable std::atomic<std::uint64_t> read_requests_small_{0U};
  mutable std::atomic<std::uint64_t> read_requests_medium_{0U};
  mutable std::atomic<std::uint64_t> read_requests_large_{0U};
  mutable std::atomic<std::uint64_t> read_requests_sequential_{0U};
  mutable std::atomic<std::uint64_t> read_requests_non_sequential_{0U};
  mutable std::atomic<std::uint64_t> large_file_small_request_reads_{0U};
  mutable std::atomic<std::uint64_t> large_file_small_request_sequential_reads_{0U};
  mutable std::atomic<std::uint64_t> open_handles_zero_reads_{0U};
  mutable std::atomic<std::uint64_t> open_handles_one_read_{0U};
  mutable std::atomic<std::uint64_t> open_handles_two_to_four_reads_{0U};
  mutable std::atomic<std::uint64_t> open_handles_five_plus_reads_{0U};
};

class MountSession {
public:
  virtual ~MountSession() = default;

  [[nodiscard]] virtual MountedVolume& mounted_volume() noexcept = 0;
  [[nodiscard]] virtual const MountedVolume& mounted_volume() const noexcept = 0;
  [[nodiscard]] virtual std::wstring_view mount_point() const noexcept = 0;
  [[nodiscard]] virtual MountSessionPerformanceStats performance_stats() const noexcept = 0;
  virtual void Stop() noexcept = 0;
};

blockio::Result<MountedVolumeHandle> OpenMountedVolume(const MountConfig& config);
blockio::Result<MountSessionHandle> StartMount(const MountConfig& config);
bool HasWinFspSupport() noexcept;
std::string_view WinFspSupportStatus() noexcept;
std::string_view ToString(MountCallbackId callback) noexcept;

} // namespace orchard::fs_winfsp
