#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "orchard/apfs/volume.h"
#include "orchard/blockio/error.h"
#include "orchard/fs_winfsp/mount.h"
#include "orchard/fs_winfsp/types.h"

namespace orchard::mount_service {

inline constexpr std::wstring_view kDefaultServiceName = L"OrchardMountService";
inline constexpr std::wstring_view kDefaultServiceDisplayName = L"Orchard Mount Service";

struct ServiceConfig {
  std::wstring service_name = std::wstring(kDefaultServiceName);
  std::wstring display_name = std::wstring(kDefaultServiceDisplayName);
  std::wstring description = L"Orchard APFS mount lifecycle service";
};

struct MountRequest {
  std::optional<std::wstring> mount_id;
  orchard::fs_winfsp::MountConfig config;
};

struct UnmountRequest {
  std::wstring mount_id;
};

struct MountedSessionPerformanceRecord {
  orchard::apfs::VolumePerformanceStats apfs;
  orchard::fs_winfsp::MountedVolumePerformanceStats mounted_volume;
  orchard::fs_winfsp::MountSessionPerformanceStats callbacks;
};

struct MountedSessionRecord {
  std::wstring mount_id;
  std::filesystem::path target_path;
  std::wstring mount_point;
  std::uint64_t volume_object_id = 0;
  std::string volume_name;
  std::wstring volume_label;
  bool read_only = true;
  MountedSessionPerformanceRecord performance;
};

blockio::Error MakeMountServiceError(blockio::ErrorCode code, std::string message,
                                     std::uint32_t system_code = 0);

} // namespace orchard::mount_service
