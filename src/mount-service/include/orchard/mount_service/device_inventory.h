#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orchard/apfs/policy.h"
#include "orchard/blockio/error.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/mount_service/device_enumerator.h"

namespace orchard::mount_service {

struct MountedVolumeBinding {
  std::wstring mount_id;
  std::wstring mount_point;
  bool read_only = true;
};

struct KnownVolumeRecord {
  std::uint64_t object_id = 0;
  std::string name;
  orchard::apfs::MountDisposition policy_action = orchard::apfs::MountDisposition::kReject;
  std::vector<std::string> policy_reasons;
  std::string policy_summary;
  std::optional<MountedVolumeBinding> mount;
  std::optional<blockio::Error> mount_error;
};

struct KnownDeviceRecord {
  std::wstring device_path;
  blockio::InspectionTargetInfo target_info;
  std::vector<KnownVolumeRecord> volumes;
  std::optional<blockio::Error> probe_error;
};

struct DeviceInventoryDiff {
  std::vector<std::wstring> added_paths;
  std::vector<std::wstring> removed_paths;
  std::vector<std::wstring> retained_paths;
};

std::wstring NormalizeDevicePathKey(std::wstring_view device_path);

class DeviceInventory {
public:
  [[nodiscard]] DeviceInventoryDiff
  DiffAgainst(const std::vector<DeviceInterfaceInfo>& present_interfaces) const;

  void UpsertDevice(KnownDeviceRecord record);
  [[nodiscard]] std::optional<KnownDeviceRecord> FindDevice(std::wstring_view device_path) const;
  [[nodiscard]] std::optional<KnownDeviceRecord> RemoveDevice(std::wstring_view device_path);
  [[nodiscard]] std::vector<KnownDeviceRecord> ListDevices() const;

  void AttachMount(std::wstring_view device_path, std::uint64_t volume_object_id,
                   const MountedVolumeBinding& binding);
  void SetMountError(std::wstring_view device_path, std::uint64_t volume_object_id,
                     const blockio::Error& error);
  void DetachMountById(std::wstring_view mount_id);

  [[nodiscard]] std::size_t MountedVolumeCountForDevice(std::wstring_view device_path) const;
  [[nodiscard]] std::vector<std::wstring> ActiveMountPoints() const;

private:
  struct DeviceEntry {
    std::wstring normalized_path;
    KnownDeviceRecord record;
  };

  std::vector<DeviceEntry> devices_;
};

} // namespace orchard::mount_service
