#include "orchard/fs_winfsp/filesystem.h"

#include <winfsp/winfsp.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "orchard/fs_winfsp/directory_query.h"
#include "orchard/fs_winfsp/file_info.h"
#include "orchard/fs_winfsp/path_bridge.h"
#include "orchard/fs_winfsp/reparse.h"

namespace orchard::fs_winfsp {
namespace {

constexpr ULONG kMetadataTimeoutMs = 10000;

NTSTATUS MapErrorToNtStatus(const blockio::Error& error) noexcept {
  switch (error.code) {
  case blockio::ErrorCode::kInvalidArgument:
    return STATUS_INVALID_PARAMETER;
  case blockio::ErrorCode::kNotFound:
    return STATUS_OBJECT_NAME_NOT_FOUND;
  case blockio::ErrorCode::kUnsupportedTarget:
  case blockio::ErrorCode::kNotImplemented:
    return STATUS_NOT_SUPPORTED;
  case blockio::ErrorCode::kAccessDenied:
    return STATUS_ACCESS_DENIED;
  case blockio::ErrorCode::kOpenFailed:
    return STATUS_OPEN_FAILED;
  case blockio::ErrorCode::kReadFailed:
  case blockio::ErrorCode::kShortRead:
  case blockio::ErrorCode::kIoctlFailed:
    return STATUS_IO_DEVICE_ERROR;
  case blockio::ErrorCode::kOutOfRange:
    return STATUS_END_OF_FILE;
  case blockio::ErrorCode::kInvalidFormat:
    return STATUS_UNRECOGNIZED_VOLUME;
  case blockio::ErrorCode::kCorruptData:
    return STATUS_FILE_CORRUPT_ERROR;
  }

  return STATUS_UNSUCCESSFUL;
}

UINT64 GetCurrentSystemTime() noexcept {
  FILETIME file_time{};
  ::GetSystemTimeAsFileTime(&file_time);
  ULARGE_INTEGER value{};
  value.LowPart = file_time.dwLowDateTime;
  value.HighPart = file_time.dwHighDateTime;
  return value.QuadPart;
}

NTSTATUS ReadOnlyNtStatus() noexcept {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS CopyOwnedBuffer(const std::vector<std::uint8_t>& source, PVOID buffer,
                         PSIZE_T size) noexcept {
  if (size == nullptr) {
    return STATUS_INVALID_PARAMETER;
  }
  if (buffer == nullptr || *size < source.size()) {
    *size = source.size();
    return STATUS_BUFFER_TOO_SMALL;
  }

  if (!source.empty()) {
    std::memcpy(buffer, source.data(), source.size());
  }
  *size = source.size();
  return STATUS_SUCCESS;
}

void CopyBasicFileInfo(const BasicFileInfo& source, FSP_FSCTL_FILE_INFO& destination) noexcept {
  std::memset(&destination, 0, sizeof(destination));
  destination.FileAttributes = source.file_attributes;
  destination.ReparseTag = source.reparse_tag;
  destination.AllocationSize = source.allocation_size;
  destination.FileSize = source.file_size;
  destination.CreationTime = source.creation_time;
  destination.LastAccessTime = source.last_access_time;
  destination.LastWriteTime = source.last_write_time;
  destination.ChangeTime = source.change_time;
  destination.IndexNumber = source.index_number;
  destination.HardLinks = source.hard_links;
  destination.EaSize = source.ea_size;
}

void PopulateDirInfo(const DirectoryQueryEntry& source, FSP_FSCTL_DIR_INFO& destination) noexcept {
  std::memset(&destination, 0, sizeof(destination));
  destination.Size =
      static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + source.file_name.size() * sizeof(WCHAR));
  CopyBasicFileInfo(source.file_info, destination.FileInfo);
  if (!source.file_name.empty()) {
    std::memcpy(destination.FileNameBuf, source.file_name.data(),
                source.file_name.size() * sizeof(WCHAR));
  }
}

std::string ComposeChildPath(const std::string_view parent, const std::string_view relative_path) {
  if (relative_path.empty() || relative_path == "/") {
    return std::string(parent);
  }
  if (parent.empty() || parent == "/") {
    return std::string(relative_path);
  }

  std::string path(parent);
  if (!path.empty() && path.back() == '/' && !relative_path.empty() &&
      relative_path.front() == '/') {
    path.pop_back();
  }
  path.append(relative_path);
  return path;
}

std::wstring LeafNameFromWindowsRelativePath(const std::wstring_view relative_path) {
  if (relative_path.empty()) {
    return std::wstring{};
  }

  const auto separator = relative_path.find_last_of(L"\\/");
  if (separator == std::wstring_view::npos) {
    return std::wstring(relative_path);
  }
  return std::wstring(relative_path.substr(separator + 1U));
}

std::string_view LeafNameFromOrchardPath(const std::string_view path) {
  if (path.empty() || path == "/") {
    return std::string_view{};
  }

  const auto separator = path.find_last_of('/');
  if (separator == std::string_view::npos) {
    return path;
  }
  return path.substr(separator + 1U);
}

struct CallbackPerformanceState {
  std::atomic<std::uint64_t> call_count{0U};
  std::atomic<std::uint64_t> total_microseconds{0U};
  std::atomic<std::uint64_t> max_microseconds{0U};
  std::atomic<std::uint64_t> slow_over_50ms{0U};
  std::atomic<std::uint64_t> slow_over_200ms{0U};
};

struct OpenFileContext {
  std::shared_ptr<FileNode> node;
  std::uint64_t read_call_count = 0;
  std::uint64_t last_read_end_offset = 0;
  bool has_last_read = false;
};

class WinFspMountSession final : public MountSession {
public:
  static blockio::Result<MountSessionHandle> Start(const MountConfig& config,
                                                   MountedVolumeHandle mounted_volume);

  ~WinFspMountSession() override {
    Stop();
  }

  [[nodiscard]] MountedVolume& mounted_volume() noexcept override {
    return *mounted_volume_;
  }

  [[nodiscard]] const MountedVolume& mounted_volume() const noexcept override {
    return *mounted_volume_;
  }

  [[nodiscard]] std::wstring_view mount_point() const noexcept override {
    return mount_point_;
  }
  [[nodiscard]] MountSessionPerformanceStats performance_stats() const noexcept override;

  void Stop() noexcept override {
    if (stopped_.exchange(true)) {
      return;
    }

    if (file_system_ != nullptr) {
      ::FspFileSystemStopDispatcher(file_system_);
      ::FspFileSystemRemoveMountPoint(file_system_);
      ::FspFileSystemDelete(file_system_);
      file_system_ = nullptr;
    }
  }

  [[nodiscard]] blockio::Result<std::shared_ptr<FileNode>>
  AcquireNodeFromFileContext(PVOID file_context) const {
    if (file_context == nullptr) {
      return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                          "Missing WinFsp file context.");
    }

    const auto* open_context = static_cast<const OpenFileContext*>(file_context);
    if (!open_context->node) {
      return orchard::apfs::MakeApfsError(blockio::ErrorCode::kNotFound,
                                          "The WinFsp file context no longer owns an open node.");
    }
    return open_context->node;
  }

  [[nodiscard]] blockio::Result<std::shared_ptr<const std::vector<DirectoryQueryEntry>>>
  GetDirectoryQueryEntries(const FileNode& directory_node) const;
  void RecordCallbackSample(MountCallbackId callback, std::uint64_t elapsed_microseconds) noexcept;

private:
  WinFspMountSession(MountedVolumeHandle mounted_volume, std::wstring mount_point) noexcept
      : mounted_volume_(std::move(mounted_volume)), mount_point_(std::move(mount_point)) {}

  static WinFspMountSession* FromFileSystem(FSP_FILE_SYSTEM* file_system) noexcept {
    return static_cast<WinFspMountSession*>(file_system->UserContext);
  }

  static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* file_system,
                                FSP_FSCTL_VOLUME_INFO* volume_info) noexcept;
  static NTSTATUS SetVolumeLabel(FSP_FILE_SYSTEM* file_system, PWSTR volume_label,
                                 FSP_FSCTL_VOLUME_INFO* volume_info) noexcept;
  static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                                    PUINT32 file_attributes,
                                    PSECURITY_DESCRIPTOR security_descriptor,
                                    SIZE_T* security_descriptor_size) noexcept;
  static NTSTATUS Create(FSP_FILE_SYSTEM* file_system, PWSTR file_name, UINT32 create_options,
                         UINT32 granted_access, UINT32 file_attributes,
                         PSECURITY_DESCRIPTOR security_descriptor, UINT64 allocation_size,
                         PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS Open(FSP_FILE_SYSTEM* file_system, PWSTR file_name, UINT32 create_options,
                       UINT32 granted_access, PVOID* file_context,
                       FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS Overwrite(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                            UINT32 file_attributes, BOOLEAN replace_file_attributes,
                            UINT64 allocation_size, FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static VOID Cleanup(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
                      ULONG flags) noexcept;
  static VOID Close(FSP_FILE_SYSTEM* file_system, PVOID file_context) noexcept;
  static NTSTATUS Read(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer,
                       UINT64 offset, ULONG length, PULONG bytes_transferred) noexcept;
  static NTSTATUS Write(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer,
                        UINT64 offset, ULONG length, BOOLEAN write_to_end_of_file,
                        BOOLEAN constrained_io, PULONG bytes_transferred,
                        FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS Flush(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                        FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                              FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                               UINT32 file_attributes, UINT64 creation_time,
                               UINT64 last_access_time, UINT64 last_write_time, UINT64 change_time,
                               FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS SetFileSize(FSP_FILE_SYSTEM* file_system, PVOID file_context, UINT64 new_size,
                              BOOLEAN set_allocation_size, FSP_FSCTL_FILE_INFO* file_info) noexcept;
  static NTSTATUS CanDelete(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                            PWSTR file_name) noexcept;
  static NTSTATUS Rename(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
                         PWSTR new_file_name, BOOLEAN replace_if_exists) noexcept;
  static NTSTATUS GetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                              PSECURITY_DESCRIPTOR security_descriptor,
                              SIZE_T* security_descriptor_size) noexcept;
  static NTSTATUS SetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                              SECURITY_INFORMATION security_information,
                              PSECURITY_DESCRIPTOR modification_descriptor) noexcept;
  static NTSTATUS ResolveReparsePoints(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                                       UINT32 reparse_point_index,
                                       BOOLEAN resolve_last_path_component,
                                       PIO_STATUS_BLOCK io_status, PVOID buffer,
                                       PSIZE_T size) noexcept;
  static NTSTATUS GetReparsePointByName(FSP_FILE_SYSTEM* file_system, PVOID context,
                                        PWSTR file_name, BOOLEAN is_directory, PVOID buffer,
                                        PSIZE_T size) noexcept;
  static NTSTATUS GetReparsePoint(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
                                  PVOID buffer, PSIZE_T size) noexcept;
  static NTSTATUS SetReparsePoint(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
                                  PVOID buffer, SIZE_T size) noexcept;
  static NTSTATUS DeleteReparsePoint(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                     PWSTR file_name, PVOID buffer, SIZE_T size) noexcept;
  static NTSTATUS GetStreamInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer,
                                ULONG length, PULONG bytes_transferred) noexcept;
  static NTSTATUS GetDirInfoByName(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                   PWSTR file_name, FSP_FSCTL_DIR_INFO* dir_info) noexcept;
  static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR pattern,
                                PWSTR marker, PVOID buffer, ULONG length,
                                PULONG bytes_transferred) noexcept;

  MountedVolumeHandle mounted_volume_;
  FSP_FILE_SYSTEM* file_system_ = nullptr;
  std::wstring mount_point_;
  std::atomic<bool> stopped_ = false;
  std::array<CallbackPerformanceState, kMountCallbackCount> callback_performance_{};
  mutable std::mutex directory_query_cache_mutex_;
  mutable std::unordered_map<std::uint64_t, std::shared_ptr<const std::vector<DirectoryQueryEntry>>>
      directory_query_entries_by_inode_;
};

class ScopedCallbackTimer {
public:
  ScopedCallbackTimer(WinFspMountSession& session, const MountCallbackId callback) noexcept
      : session_(session), callback_(callback), started_at_(std::chrono::steady_clock::now()) {}

  ~ScopedCallbackTimer() {
    const auto finished_at = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(finished_at - started_at_).count();
    session_.RecordCallbackSample(callback_, static_cast<std::uint64_t>(elapsed));
  }

private:
  WinFspMountSession& session_;
  MountCallbackId callback_;
  std::chrono::steady_clock::time_point started_at_;
};

void PopulateVolumeInfo(const MountedVolume& mounted_volume,
                        FSP_FSCTL_VOLUME_INFO& volume_info) noexcept {
  std::memset(&volume_info, 0, sizeof(volume_info));
  volume_info.TotalSize = mounted_volume.container_info().byte_length;
  volume_info.FreeSize = 0U;

  const auto& label = mounted_volume.volume_label();
  const auto label_chars = std::min<std::size_t>(label.size(), 32U);
  volume_info.VolumeLabelLength = static_cast<UINT16>(label_chars * sizeof(WCHAR));
  if (label_chars > 0U) {
    std::memcpy(volume_info.VolumeLabel, label.data(), label_chars * sizeof(WCHAR));
  }
}

blockio::Result<FileNode> ResolveNodeFromWidePath(const MountedVolume& mounted_volume,
                                                  const std::wstring_view file_name) {
  auto normalized_path_result = NormalizeWindowsPath(file_name);
  if (!normalized_path_result.ok()) {
    return normalized_path_result.error();
  }

  return mounted_volume.ResolveFileNode(normalized_path_result.value());
}

blockio::Result<std::vector<std::uint8_t>> ReadFileBytes(const MountedVolume& mounted_volume,
                                                         const FileNode& node,
                                                         orchard::apfs::FileReadRequest request) {
  return mounted_volume.ReadFileRange(node, request);
}

blockio::Result<std::shared_ptr<const std::vector<DirectoryQueryEntry>>>
WinFspMountSession::GetDirectoryQueryEntries(const FileNode& directory_node) const {
  if (directory_node.metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Directory query requested for a non-directory node.");
  }

  {
    std::scoped_lock lock(directory_query_cache_mutex_);
    const auto existing = directory_query_entries_by_inode_.find(directory_node.inode_id);
    if (existing != directory_query_entries_by_inode_.end()) {
      return existing->second;
    }
  }

  auto entries_view_result = mounted_volume().GetDirectoryEntriesView(directory_node.inode_id);
  if (!entries_view_result.ok()) {
    return entries_view_result.error();
  }
  mounted_volume().PrimeDirectoryChildren(directory_node, *entries_view_result.value());

  auto query_entries_result =
      BuildDirectoryQueryEntries(mounted_volume(), directory_node, *entries_view_result.value());
  if (!query_entries_result.ok()) {
    return query_entries_result.error();
  }
  auto query_entries = std::make_shared<const std::vector<DirectoryQueryEntry>>(
      std::move(query_entries_result.value()));

  {
    std::scoped_lock lock(directory_query_cache_mutex_);
    auto [entry, inserted] =
        directory_query_entries_by_inode_.try_emplace(directory_node.inode_id, query_entries);
    if (inserted) {
      return entry->second;
    }
    return entry->second;
  }
}

MountSessionPerformanceStats WinFspMountSession::performance_stats() const noexcept {
  MountSessionPerformanceStats stats;
  for (std::size_t index = 0; index < callback_performance_.size(); ++index) {
    stats.callbacks[index].call_count = callback_performance_[index].call_count.load();
    stats.callbacks[index].total_microseconds =
        callback_performance_[index].total_microseconds.load();
    stats.callbacks[index].max_microseconds = callback_performance_[index].max_microseconds.load();
    stats.callbacks[index].slow_over_50ms = callback_performance_[index].slow_over_50ms.load();
    stats.callbacks[index].slow_over_200ms = callback_performance_[index].slow_over_200ms.load();
  }
  return stats;
}

void WinFspMountSession::RecordCallbackSample(const MountCallbackId callback,
                                              const std::uint64_t elapsed_microseconds) noexcept {
  auto& state = callback_performance_[static_cast<std::size_t>(callback)];
  ++state.call_count;
  state.total_microseconds.fetch_add(elapsed_microseconds);

  auto previous_max = state.max_microseconds.load();
  while (previous_max < elapsed_microseconds &&
         !state.max_microseconds.compare_exchange_weak(previous_max, elapsed_microseconds)) {
  }

  if (elapsed_microseconds >= 50'000U) {
    ++state.slow_over_50ms;
  }
  if (elapsed_microseconds >= 200'000U) {
    ++state.slow_over_200ms;
  }
}

blockio::Result<MountSessionHandle> WinFspMountSession::Start(const MountConfig& config,
                                                              MountedVolumeHandle mounted_volume) {
  static const FSP_FILE_SYSTEM_INTERFACE file_system_interface = [] {
    FSP_FILE_SYSTEM_INTERFACE value{};
    value.GetVolumeInfo = &WinFspMountSession::GetVolumeInfo;
    value.SetVolumeLabel = &WinFspMountSession::SetVolumeLabel;
    value.GetSecurityByName = &WinFspMountSession::GetSecurityByName;
    value.Create = &WinFspMountSession::Create;
    value.Open = &WinFspMountSession::Open;
    value.Overwrite = &WinFspMountSession::Overwrite;
    value.Cleanup = &WinFspMountSession::Cleanup;
    value.Close = &WinFspMountSession::Close;
    value.Read = &WinFspMountSession::Read;
    value.Write = &WinFspMountSession::Write;
    value.Flush = &WinFspMountSession::Flush;
    value.GetFileInfo = &WinFspMountSession::GetFileInfo;
    value.SetBasicInfo = &WinFspMountSession::SetBasicInfo;
    value.SetFileSize = &WinFspMountSession::SetFileSize;
    value.CanDelete = &WinFspMountSession::CanDelete;
    value.Rename = &WinFspMountSession::Rename;
    value.GetSecurity = &WinFspMountSession::GetSecurity;
    value.SetSecurity = &WinFspMountSession::SetSecurity;
    value.ResolveReparsePoints = &WinFspMountSession::ResolveReparsePoints;
    value.GetReparsePoint = &WinFspMountSession::GetReparsePoint;
    value.SetReparsePoint = &WinFspMountSession::SetReparsePoint;
    value.DeleteReparsePoint = &WinFspMountSession::DeleteReparsePoint;
    value.GetStreamInfo = &WinFspMountSession::GetStreamInfo;
    value.GetDirInfoByName = &WinFspMountSession::GetDirInfoByName;
    value.ReadDirectory = &WinFspMountSession::ReadDirectory;
    return value;
  }();

  auto session = std::unique_ptr<WinFspMountSession>(
      new WinFspMountSession(std::move(mounted_volume), config.mount_point));

  FSP_FSCTL_VOLUME_PARAMS volume_params{};
  volume_params.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
  volume_params.SectorSize = session->mounted_volume().container_info().block_size;
  volume_params.SectorsPerAllocationUnit = 1;
  volume_params.VolumeCreationTime = GetCurrentSystemTime();
  volume_params.VolumeSerialNumber = session->mounted_volume().volume_serial_number();
  volume_params.FileInfoTimeout = kMetadataTimeoutMs;
  volume_params.VolumeInfoTimeoutValid = 1U;
  volume_params.VolumeInfoTimeout = kMetadataTimeoutMs;
  volume_params.DirInfoTimeoutValid = 1U;
  volume_params.DirInfoTimeout = kMetadataTimeoutMs;
  volume_params.SecurityTimeoutValid = 1U;
  volume_params.SecurityTimeout = kMetadataTimeoutMs;
  volume_params.StreamInfoTimeoutValid = 1U;
  volume_params.StreamInfoTimeout = kMetadataTimeoutMs;
  volume_params.CaseSensitiveSearch =
      session->mounted_volume().volume_info().case_insensitive ? 0U : 1U;
  volume_params.CasePreservedNames = 1;
  volume_params.UnicodeOnDisk = 1;
  volume_params.PersistentAcls = 1;
  volume_params.ReadOnlyVolume = 1;
  volume_params.PassQueryDirectoryFileName = 1U;
  volume_params.AllowOpenInKernelMode = 1;
  ::wcscpy_s(volume_params.FileSystemName, sizeof(volume_params.FileSystemName) / sizeof(WCHAR),
             L"Orchard");

  auto status =
      ::FspFileSystemCreate(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &volume_params,
                            &file_system_interface, &session->file_system_);
  if (!NT_SUCCESS(status)) {
    return orchard::apfs::MakeApfsError(
        blockio::ErrorCode::kOpenFailed,
        "FspFileSystemCreate failed for the Orchard WinFsp adapter.");
  }

  session->file_system_->UserContext = session.get();

  status = ::FspFileSystemSetMountPoint(session->file_system_, session->mount_point_.empty()
                                                                   ? nullptr
                                                                   : session->mount_point_.data());
  if (!NT_SUCCESS(status)) {
    ::FspFileSystemDelete(session->file_system_);
    session->file_system_ = nullptr;
    return orchard::apfs::MakeApfsError(
        blockio::ErrorCode::kOpenFailed,
        "FspFileSystemSetMountPoint failed for the Orchard WinFsp adapter.");
  }

  status = ::FspFileSystemStartDispatcher(session->file_system_, 0);
  if (!NT_SUCCESS(status)) {
    ::FspFileSystemRemoveMountPoint(session->file_system_);
    ::FspFileSystemDelete(session->file_system_);
    session->file_system_ = nullptr;
    return orchard::apfs::MakeApfsError(
        blockio::ErrorCode::kOpenFailed,
        "FspFileSystemStartDispatcher failed for the Orchard WinFsp adapter.");
  }

  MountSessionHandle handle(session.release());
  return handle;
}

NTSTATUS WinFspMountSession::GetVolumeInfo(FSP_FILE_SYSTEM* file_system,
                                           FSP_FSCTL_VOLUME_INFO* volume_info) noexcept {
  PopulateVolumeInfo(FromFileSystem(file_system)->mounted_volume(), *volume_info);
  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::SetVolumeLabel(FSP_FILE_SYSTEM* file_system, PWSTR volume_label,
                                            FSP_FSCTL_VOLUME_INFO* volume_info) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(volume_label);
  static_cast<void>(volume_info);
  return ReadOnlyNtStatus();
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
NTSTATUS WinFspMountSession::GetSecurityByName(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                                               PUINT32 file_attributes,
                                               PSECURITY_DESCRIPTOR security_descriptor,
                                               SIZE_T* security_descriptor_size) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kGetSecurityByName);
  auto node_result = ResolveNodeFromWidePath(session->mounted_volume(), file_name);
  if (!node_result.ok()) {
    return MapErrorToNtStatus(node_result.error());
  }

  const auto basic_info = BuildBasicFileInfo(
      node_result.value(), session->mounted_volume().volume_context().block_size());
  if (file_attributes != nullptr) {
    *file_attributes = basic_info.file_attributes;
  }

  const auto& descriptor = session->mounted_volume().security_descriptor();
  if (security_descriptor_size != nullptr) {
    if (*security_descriptor_size < descriptor.size()) {
      *security_descriptor_size = descriptor.size();
      return STATUS_BUFFER_OVERFLOW;
    }

    *security_descriptor_size = descriptor.size();
    if (security_descriptor != nullptr && !descriptor.empty()) {
      std::memcpy(security_descriptor, descriptor.data(), descriptor.size());
    }
  }

  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::Create(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                                    UINT32 create_options, UINT32 granted_access,
                                    UINT32 file_attributes,
                                    PSECURITY_DESCRIPTOR security_descriptor,
                                    UINT64 allocation_size, PVOID* file_context,
                                    FSP_FSCTL_FILE_INFO* file_info) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kCreate);
  static_cast<void>(file_system);
  static_cast<void>(file_name);
  static_cast<void>(create_options);
  static_cast<void>(granted_access);
  static_cast<void>(file_attributes);
  static_cast<void>(security_descriptor);
  static_cast<void>(allocation_size);
  static_cast<void>(file_context);
  static_cast<void>(file_info);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::Open(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                                  const UINT32 create_options, const UINT32 granted_access,
                                  PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) noexcept {
  static_cast<void>(granted_access);
  if (file_context == nullptr || file_info == nullptr) {
    return STATUS_INVALID_PARAMETER;
  }

  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kOpen);
  auto normalized_path_result = NormalizeWindowsPath(file_name);
  if (!normalized_path_result.ok()) {
    return MapErrorToNtStatus(normalized_path_result.error());
  }

  auto node_result = session->mounted_volume().AcquireOpenNode(normalized_path_result.value());
  if (!node_result.ok()) {
    return MapErrorToNtStatus(node_result.error());
  }

  const auto& node = *node_result.value();
  if ((create_options & FILE_DIRECTORY_FILE) != 0U &&
      node.metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    session->mounted_volume().ReleaseOpenNode(node_result.value().get());
    return STATUS_NOT_A_DIRECTORY;
  }
  if ((create_options & FILE_NON_DIRECTORY_FILE) != 0U &&
      node.metadata.kind == orchard::apfs::InodeKind::kDirectory) {
    session->mounted_volume().ReleaseOpenNode(node_result.value().get());
    return STATUS_FILE_IS_A_DIRECTORY;
  }

  auto* open_context = new (std::nothrow) OpenFileContext{
      .node = node_result.value(),
  };
  if (open_context == nullptr) {
    session->mounted_volume().ReleaseOpenNode(node_result.value().get());
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  *file_context = open_context;
  CopyBasicFileInfo(
      BuildBasicFileInfo(node, session->mounted_volume().volume_context().block_size()),
      *file_info);

  if (session->mounted_volume().volume_info().case_insensitive) {
    auto normalized_name_result = OrchardPathToWindowsPath(node.normalized_path);
    if (normalized_name_result.ok()) {
      auto* open_file_info = ::FspFileSystemGetOpenFileInfo(file_info);
      if (open_file_info != nullptr && open_file_info->NormalizedName != nullptr) {
        const auto max_chars =
            static_cast<std::size_t>(open_file_info->NormalizedNameSize / sizeof(WCHAR));
        const auto chars_to_copy =
            std::min<std::size_t>(normalized_name_result.value().size(), max_chars);
        std::wmemcpy(open_file_info->NormalizedName, normalized_name_result.value().data(),
                     chars_to_copy);
        open_file_info->NormalizedNameSize = static_cast<UINT16>(chars_to_copy * sizeof(WCHAR));
      }
    }
  }

  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::Overwrite(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                       UINT32 file_attributes, BOOLEAN replace_file_attributes,
                                       UINT64 allocation_size,
                                       FSP_FSCTL_FILE_INFO* file_info) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_attributes);
  static_cast<void>(replace_file_attributes);
  static_cast<void>(allocation_size);
  static_cast<void>(file_info);
  return ReadOnlyNtStatus();
}

VOID WinFspMountSession::Cleanup(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
                                 ULONG flags) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_name);
  static_cast<void>(flags);
}

VOID WinFspMountSession::Close(FSP_FILE_SYSTEM* file_system, PVOID file_context) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kClose);
  auto* open_context = static_cast<OpenFileContext*>(file_context);
  if (open_context == nullptr) {
    return;
  }

  session->mounted_volume().RecordCompletedOpenReadCount(open_context->read_call_count);
  if (open_context->node) {
    session->mounted_volume().ReleaseOpenNode(open_context->node.get());
  }
  delete open_context;
}

NTSTATUS WinFspMountSession::Read(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer,
                                  const UINT64 offset, const ULONG length,
                                  PULONG bytes_transferred) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kRead);
  auto node_result = session->AcquireNodeFromFileContext(file_context);
  if (!node_result.ok()) {
    return STATUS_INVALID_HANDLE;
  }

  if (node_result.value()->metadata.kind == orchard::apfs::InodeKind::kDirectory) {
    return STATUS_FILE_IS_A_DIRECTORY;
  }
  if (node_result.value()->metadata.kind == orchard::apfs::InodeKind::kSymlink &&
      node_result.value()->symlink_reparse_eligible) {
    return STATUS_INVALID_DEVICE_REQUEST;
  }
  *bytes_transferred = 0U;
  if (length == 0U) {
    return STATUS_SUCCESS;
  }
  if (offset >= node_result.value()->metadata.logical_size) {
    return STATUS_END_OF_FILE;
  }

  orchard::apfs::FileReadRequest request;
  request.inode_id = node_result.value()->inode_id;
  request.offset = offset;
  request.size = static_cast<std::size_t>(length);
  auto bytes_result = ReadFileBytes(session->mounted_volume(), *node_result.value(), request);
  if (!bytes_result.ok()) {
    return MapErrorToNtStatus(bytes_result.error());
  }

  auto* open_context = static_cast<OpenFileContext*>(file_context);
  const bool sequential_request = open_context != nullptr && open_context->has_last_read &&
                                  offset == open_context->last_read_end_offset;
  if (open_context != nullptr) {
    ++open_context->read_call_count;
    open_context->has_last_read = true;
    open_context->last_read_end_offset = offset + bytes_result.value().size();
  }
  session->mounted_volume().RecordObservedRead(node_result.value()->metadata.logical_size,
                                               bytes_result.value().size(), sequential_request);

  std::memcpy(buffer, bytes_result.value().data(), bytes_result.value().size());
  *bytes_transferred = static_cast<ULONG>(bytes_result.value().size());
  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::Write(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer,
                                   UINT64 offset, ULONG length, BOOLEAN write_to_end_of_file,
                                   BOOLEAN constrained_io, PULONG bytes_transferred,
                                   FSP_FSCTL_FILE_INFO* file_info) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(buffer);
  static_cast<void>(offset);
  static_cast<void>(length);
  static_cast<void>(write_to_end_of_file);
  static_cast<void>(constrained_io);
  if (bytes_transferred != nullptr) {
    *bytes_transferred = 0U;
  }
  static_cast<void>(file_info);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::Flush(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                   FSP_FSCTL_FILE_INFO* file_info) noexcept {
  if (file_context == nullptr) {
    return STATUS_SUCCESS;
  }

  return GetFileInfo(file_system, file_context, file_info);
}

NTSTATUS WinFspMountSession::GetFileInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                         FSP_FSCTL_FILE_INFO* file_info) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kGetFileInfo);
  auto node_result = session->AcquireNodeFromFileContext(file_context);
  if (!node_result.ok()) {
    return STATUS_INVALID_HANDLE;
  }

  CopyBasicFileInfo(BuildBasicFileInfo(*node_result.value(),
                                       session->mounted_volume().volume_context().block_size()),
                    *file_info);
  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::SetBasicInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                          UINT32 file_attributes, UINT64 creation_time,
                                          UINT64 last_access_time, UINT64 last_write_time,
                                          UINT64 change_time,
                                          FSP_FSCTL_FILE_INFO* file_info) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_attributes);
  static_cast<void>(creation_time);
  static_cast<void>(last_access_time);
  static_cast<void>(last_write_time);
  static_cast<void>(change_time);
  static_cast<void>(file_info);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::SetFileSize(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                         UINT64 new_size, BOOLEAN set_allocation_size,
                                         FSP_FSCTL_FILE_INFO* file_info) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(new_size);
  static_cast<void>(set_allocation_size);
  static_cast<void>(file_info);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::CanDelete(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                       PWSTR file_name) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_name);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::Rename(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                    PWSTR file_name, PWSTR new_file_name,
                                    BOOLEAN replace_if_exists) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_name);
  static_cast<void>(new_file_name);
  static_cast<void>(replace_if_exists);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::GetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                         PSECURITY_DESCRIPTOR security_descriptor,
                                         SIZE_T* security_descriptor_size) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kGetSecurity);
  auto node_result = session->AcquireNodeFromFileContext(file_context);
  if (!node_result.ok()) {
    return STATUS_INVALID_HANDLE;
  }

  const auto& descriptor = session->mounted_volume().security_descriptor();
  if (security_descriptor_size == nullptr) {
    return STATUS_SUCCESS;
  }
  if (*security_descriptor_size < descriptor.size()) {
    *security_descriptor_size = descriptor.size();
    return STATUS_BUFFER_OVERFLOW;
  }

  *security_descriptor_size = descriptor.size();
  if (security_descriptor != nullptr && !descriptor.empty()) {
    std::memcpy(security_descriptor, descriptor.data(), descriptor.size());
  }
  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::SetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                         SECURITY_INFORMATION security_information,
                                         PSECURITY_DESCRIPTOR modification_descriptor) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(security_information);
  static_cast<void>(modification_descriptor);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::ResolveReparsePoints(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                                                  const UINT32 reparse_point_index,
                                                  const BOOLEAN resolve_last_path_component,
                                                  PIO_STATUS_BLOCK io_status, PVOID buffer,
                                                  PSIZE_T size) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kResolveReparsePoints);
  return ::FspFileSystemResolveReparsePoints(
      file_system, &WinFspMountSession::GetReparsePointByName, session, file_name,
      reparse_point_index, resolve_last_path_component, io_status, buffer, size);
}

NTSTATUS WinFspMountSession::GetReparsePointByName(FSP_FILE_SYSTEM* file_system, PVOID context,
                                                   PWSTR file_name, BOOLEAN is_directory,
                                                   PVOID buffer, PSIZE_T size) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(is_directory);

  auto* session = static_cast<WinFspMountSession*>(context);
  auto node_result = ResolveNodeFromWidePath(session->mounted_volume(), file_name);
  if (!node_result.ok()) {
    return MapErrorToNtStatus(node_result.error());
  }
  const auto& symlink_target_opt = node_result.value().symlink_target;
  if (node_result.value().metadata.kind != orchard::apfs::InodeKind::kSymlink ||
      !symlink_target_opt.has_value() || !node_result.value().symlink_reparse_eligible) {
    return STATUS_NOT_A_REPARSE_POINT;
  }
  const auto symlink_target = symlink_target_opt.value();

  auto reparse_result = BuildSymlinkReparseData(SymlinkReparseRequest{
      .mount_point = std::wstring(session->mount_point()),
      .target = symlink_target,
  });
  if (!reparse_result.ok()) {
    return MapErrorToNtStatus(reparse_result.error());
  }
  if (buffer == nullptr) {
    if (size != nullptr) {
      *size = reparse_result.value().size();
    }
    return STATUS_SUCCESS;
  }

  return CopyOwnedBuffer(reparse_result.value(), buffer, size);
}

NTSTATUS WinFspMountSession::GetReparsePoint(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                             PWSTR file_name, PVOID buffer, PSIZE_T size) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kGetReparsePoint);
  const auto missing_context_error = orchard::apfs::MakeApfsError(
      blockio::ErrorCode::kInvalidArgument, "Missing file context for reparse query.");
  blockio::Result<std::shared_ptr<FileNode>> node_result(missing_context_error);
  if (file_context != nullptr) {
    node_result = session->AcquireNodeFromFileContext(file_context);
  }
  if (file_context == nullptr) {
    auto normalized_path_result = NormalizeWindowsPath(file_name);
    if (!normalized_path_result.ok()) {
      return MapErrorToNtStatus(normalized_path_result.error());
    }
    node_result = session->mounted_volume().AcquireOpenNode(normalized_path_result.value());
  }
  if (!node_result.ok()) {
    return file_context != nullptr ? STATUS_INVALID_HANDLE
                                   : MapErrorToNtStatus(node_result.error());
  }
  const auto& symlink_target_opt = node_result.value()->symlink_target;
  if (node_result.value()->metadata.kind != orchard::apfs::InodeKind::kSymlink ||
      !symlink_target_opt.has_value() || !node_result.value()->symlink_reparse_eligible) {
    if (file_context == nullptr) {
      session->mounted_volume().ReleaseOpenNode(node_result.value().get());
    }
    return STATUS_NOT_A_REPARSE_POINT;
  }
  const auto symlink_target = symlink_target_opt.value();

  auto reparse_result = BuildSymlinkReparseData(SymlinkReparseRequest{
      .mount_point = std::wstring(session->mount_point()),
      .target = symlink_target,
  });
  if (file_context == nullptr) {
    session->mounted_volume().ReleaseOpenNode(node_result.value().get());
  }
  if (!reparse_result.ok()) {
    return MapErrorToNtStatus(reparse_result.error());
  }

  return CopyOwnedBuffer(reparse_result.value(), buffer, size);
}

NTSTATUS WinFspMountSession::SetReparsePoint(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                             PWSTR file_name, PVOID buffer, SIZE_T size) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_name);
  static_cast<void>(buffer);
  static_cast<void>(size);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::DeleteReparsePoint(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                                PWSTR file_name, PVOID buffer,
                                                SIZE_T size) noexcept {
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(file_name);
  static_cast<void>(buffer);
  static_cast<void>(size);
  return ReadOnlyNtStatus();
}

NTSTATUS WinFspMountSession::GetStreamInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                           PVOID buffer, ULONG length,
                                           PULONG bytes_transferred) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kGetStreamInfo);
  static_cast<void>(file_system);
  static_cast<void>(file_context);
  static_cast<void>(buffer);
  static_cast<void>(length);
  if (bytes_transferred != nullptr) {
    *bytes_transferred = 0U;
  }
  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::GetDirInfoByName(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                              PWSTR file_name,
                                              FSP_FSCTL_DIR_INFO* dir_info) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kGetDirInfoByName);
  if (file_name == nullptr || dir_info == nullptr) {
    return STATUS_INVALID_PARAMETER;
  }

  auto directory_node_result = session->AcquireNodeFromFileContext(file_context);
  if (!directory_node_result.ok()) {
    return STATUS_INVALID_HANDLE;
  }
  if (directory_node_result.value()->metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return STATUS_NOT_A_DIRECTORY;
  }

  auto normalized_child_result = NormalizeWindowsPath(file_name);
  if (!normalized_child_result.ok()) {
    return MapErrorToNtStatus(normalized_child_result.error());
  }

  const auto& normalized_child = normalized_child_result.value();
  if (normalized_child.size() <= 1U || normalized_child[0] != '/') {
    return STATUS_INVALID_PARAMETER;
  }

  if (normalized_child.find('/', 1U) == std::string::npos) {
    const auto child_name = normalized_child.substr(1U);
    auto wide_child_result = Utf8ToWide(child_name);
    if (!wide_child_result.ok()) {
      return MapErrorToNtStatus(wide_child_result.error());
    }

    auto query_entries_result = session->GetDirectoryQueryEntries(*directory_node_result.value());
    if (!query_entries_result.ok()) {
      return MapErrorToNtStatus(query_entries_result.error());
    }

    const auto entry_it =
        std::find_if(query_entries_result.value()->begin(), query_entries_result.value()->end(),
                     [&session, &wide_child_result](const DirectoryQueryEntry& entry) {
                       return CompareDirectoryNames(
                                  entry.file_name, wide_child_result.value(),
                                  session->mounted_volume().volume_info().case_insensitive) == 0;
                     });
    if (entry_it != query_entries_result.value()->end()) {
      PopulateDirInfo(*entry_it, *dir_info);
      return STATUS_SUCCESS;
    }
  }

  auto node_result = session->mounted_volume().ResolveFileNode(
      ComposeChildPath(directory_node_result.value()->normalized_path, normalized_child));
  if (!node_result.ok()) {
    return MapErrorToNtStatus(node_result.error());
  }

  auto file_name_result = Utf8ToWide(LeafNameFromOrchardPath(node_result.value().normalized_path));
  if (!file_name_result.ok()) {
    auto fallback_name = LeafNameFromWindowsRelativePath(file_name);
    PopulateDirInfo(
        DirectoryQueryEntry{
            .file_name = std::move(fallback_name),
            .file_info = BuildBasicFileInfo(
                node_result.value(), session->mounted_volume().volume_context().block_size()),
        },
        *dir_info);
    return STATUS_SUCCESS;
  }

  PopulateDirInfo(
      DirectoryQueryEntry{
          .file_name = std::move(file_name_result.value()),
          .file_info = BuildBasicFileInfo(node_result.value(),
                                          session->mounted_volume().volume_context().block_size()),
      },
      *dir_info);
  return STATUS_SUCCESS;
}

NTSTATUS WinFspMountSession::ReadDirectory(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                                           PWSTR pattern, PWSTR marker, PVOID buffer,
                                           const ULONG length, PULONG bytes_transferred) noexcept {
  auto* session = FromFileSystem(file_system);
  ScopedCallbackTimer timer(*session, MountCallbackId::kReadDirectory);
  auto directory_node_result = session->AcquireNodeFromFileContext(file_context);
  if (!directory_node_result.ok()) {
    return STATUS_INVALID_HANDLE;
  }
  if (directory_node_result.value()->metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return STATUS_NOT_A_DIRECTORY;
  }

  *bytes_transferred = 0U;
  auto query_entries_result = session->GetDirectoryQueryEntries(*directory_node_result.value());
  if (!query_entries_result.ok()) {
    return MapErrorToNtStatus(query_entries_result.error());
  }

  DirectoryQueryRequest request;
  request.case_insensitive = session->mounted_volume().volume_info().case_insensitive;
  request.pattern = pattern != nullptr ? std::wstring(pattern) : std::wstring(L"*");
  if (marker != nullptr) {
    request.marker = std::wstring(marker);
  }

  const auto query_page =
      PaginateFilteredDirectoryQueryEntries(*query_entries_result.value(), request,
                                            DirectoryQueryPaginationConfig{
                                                .max_bytes = length,
                                                .base_entry_size = sizeof(FSP_FSCTL_DIR_INFO),
                                            });
  if (query_page.entries.empty() && query_page.truncated) {
    return STATUS_BUFFER_TOO_SMALL;
  }

  for (const auto& query_entry : query_page.entries) {
    std::vector<std::uint8_t> dir_info_bytes(
        sizeof(FSP_FSCTL_DIR_INFO) + query_entry.file_name.size() * sizeof(WCHAR), 0U);
    auto* dir_info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(dir_info_bytes.data());
    PopulateDirInfo(query_entry, *dir_info);

    if (!::FspFileSystemAddDirInfo(dir_info, buffer, length, bytes_transferred)) {
      return STATUS_SUCCESS;
    }
  }

  ::FspFileSystemAddDirInfo(nullptr, buffer, length, bytes_transferred);
  return STATUS_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

blockio::Result<MountSessionHandle>
CreateFileSystemSession(const MountConfig& config, const MountedVolumeHandle& mounted_volume) {
  return WinFspMountSession::Start(config, mounted_volume);
}

bool IsWinFspBackendAvailable() noexcept {
  return true;
}

std::string_view WinFspBackendStatus() noexcept {
  return "available";
}

} // namespace orchard::fs_winfsp
