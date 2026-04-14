#include "orchard/mount_service/device_inventory.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace orchard::mount_service {
namespace {

std::wstring NormalizeMountPointKey(std::wstring_view mount_point) {
  std::wstring normalized(mount_point);
  for (auto& ch : normalized) {
    ch = static_cast<wchar_t>(std::towupper(ch));
  }
  return normalized;
}

} // namespace

std::wstring NormalizeDevicePathKey(const std::wstring_view device_path) {
  std::wstring normalized(device_path);
  for (auto& ch : normalized) {
    ch = static_cast<wchar_t>(std::towupper(ch));
  }
  return normalized;
}

DeviceInventoryDiff
DeviceInventory::DiffAgainst(const std::vector<DeviceInterfaceInfo>& present_interfaces) const {
  DeviceInventoryDiff diff;

  for (const auto& device : present_interfaces) {
    const auto normalized_path = NormalizeDevicePathKey(device.device_path);
    const auto existing =
        std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
          return entry.normalized_path == normalized_path;
        });
    if (existing == devices_.end()) {
      diff.added_paths.push_back(device.device_path);
    } else {
      diff.retained_paths.push_back(device.device_path);
    }
  }

  for (const auto& entry : devices_) {
    const auto present =
        std::find_if(present_interfaces.begin(), present_interfaces.end(),
                     [&](const DeviceInterfaceInfo& device) {
                       return NormalizeDevicePathKey(device.device_path) == entry.normalized_path;
                     });
    if (present == present_interfaces.end()) {
      diff.removed_paths.push_back(entry.record.device_path);
    }
  }

  const auto by_path = [](const std::wstring& left, const std::wstring& right) {
    return NormalizeDevicePathKey(left) < NormalizeDevicePathKey(right);
  };
  std::sort(diff.added_paths.begin(), diff.added_paths.end(), by_path);
  std::sort(diff.removed_paths.begin(), diff.removed_paths.end(), by_path);
  std::sort(diff.retained_paths.begin(), diff.retained_paths.end(), by_path);
  return diff;
}

void DeviceInventory::UpsertDevice(KnownDeviceRecord record) {
  const auto normalized_path = NormalizeDevicePathKey(record.device_path);
  const auto existing =
      std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
        return entry.normalized_path == normalized_path;
      });

  if (existing == devices_.end()) {
    devices_.push_back(DeviceEntry{
        .normalized_path = normalized_path,
        .record = std::move(record),
    });
    return;
  }

  existing->record = std::move(record);
}

std::optional<KnownDeviceRecord>
DeviceInventory::FindDevice(const std::wstring_view device_path) const {
  const auto normalized_path = NormalizeDevicePathKey(device_path);
  const auto existing =
      std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
        return entry.normalized_path == normalized_path;
      });
  if (existing == devices_.end()) {
    return std::nullopt;
  }

  return existing->record;
}

std::optional<KnownDeviceRecord>
DeviceInventory::RemoveDevice(const std::wstring_view device_path) {
  const auto normalized_path = NormalizeDevicePathKey(device_path);
  const auto existing =
      std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
        return entry.normalized_path == normalized_path;
      });
  if (existing == devices_.end()) {
    return std::nullopt;
  }

  auto record = std::move(existing->record);
  devices_.erase(existing);
  return record;
}

std::vector<KnownDeviceRecord> DeviceInventory::ListDevices() const {
  std::vector<KnownDeviceRecord> devices;
  devices.reserve(devices_.size());
  for (const auto& entry : devices_) {
    devices.push_back(entry.record);
  }

  std::sort(devices.begin(), devices.end(),
            [](const KnownDeviceRecord& left, const KnownDeviceRecord& right) {
              return NormalizeDevicePathKey(left.device_path) <
                     NormalizeDevicePathKey(right.device_path);
            });
  return devices;
}

void DeviceInventory::AttachMount(const std::wstring_view device_path,
                                  const std::uint64_t volume_object_id,
                                  const MountedVolumeBinding& binding) {
  const auto normalized_path = NormalizeDevicePathKey(device_path);
  const auto existing =
      std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
        return entry.normalized_path == normalized_path;
      });
  if (existing == devices_.end()) {
    return;
  }

  for (auto& volume : existing->record.volumes) {
    if (volume.object_id == volume_object_id) {
      volume.mount = binding;
      volume.mount_error.reset();
      return;
    }
  }
}

void DeviceInventory::SetMountError(const std::wstring_view device_path,
                                    const std::uint64_t volume_object_id,
                                    const blockio::Error& error) {
  const auto normalized_path = NormalizeDevicePathKey(device_path);
  const auto existing =
      std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
        return entry.normalized_path == normalized_path;
      });
  if (existing == devices_.end()) {
    return;
  }

  for (auto& volume : existing->record.volumes) {
    if (volume.object_id == volume_object_id) {
      volume.mount.reset();
      volume.mount_error = error;
      return;
    }
  }
}

void DeviceInventory::DetachMountById(const std::wstring_view mount_id) {
  for (auto& entry : devices_) {
    for (auto& volume : entry.record.volumes) {
      if (volume.mount.has_value() && volume.mount->mount_id == mount_id) {
        volume.mount.reset();
      }
    }
  }
}

std::size_t
DeviceInventory::MountedVolumeCountForDevice(const std::wstring_view device_path) const {
  const auto normalized_path = NormalizeDevicePathKey(device_path);
  const auto existing =
      std::find_if(devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
        return entry.normalized_path == normalized_path;
      });
  if (existing == devices_.end()) {
    return 0U;
  }

  return static_cast<std::size_t>(
      std::count_if(existing->record.volumes.begin(), existing->record.volumes.end(),
                    [](const KnownVolumeRecord& volume) { return volume.mount.has_value(); }));
}

std::vector<std::wstring> DeviceInventory::ActiveMountPoints() const {
  std::vector<std::wstring> mount_points;
  for (const auto& entry : devices_) {
    for (const auto& volume : entry.record.volumes) {
      if (volume.mount.has_value()) {
        mount_points.push_back(NormalizeMountPointKey(volume.mount->mount_point));
      }
    }
  }

  std::sort(mount_points.begin(), mount_points.end());
  mount_points.erase(std::unique(mount_points.begin(), mount_points.end()), mount_points.end());
  return mount_points;
}

} // namespace orchard::mount_service
