#include "orchard/mount_service/device_discovery.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <utility>
#include <vector>

#include "orchard/apfs/discovery.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/reader.h"
#include "orchard/fs_winfsp/types.h"
#include "orchard/mount_service/types.h"

namespace orchard::mount_service {
namespace {

class DefaultDeviceProber final : public DeviceProber {
public:
  [[nodiscard]] blockio::Result<KnownDeviceRecord>
  Probe(const DeviceInterfaceInfo& device) override {
    KnownDeviceRecord record;
    record.device_path = device.device_path;
    record.target_info = blockio::InspectTargetPath(device.device_path);

    auto reader_result = blockio::OpenReader(record.target_info);
    if (!reader_result.ok()) {
      return reader_result.error();
    }

    auto discovery_result = orchard::apfs::Discover(*reader_result.value());
    if (!discovery_result.ok()) {
      return discovery_result.error();
    }

    for (const auto& container : discovery_result.value().containers) {
      for (const auto& volume : container.volumes) {
        KnownVolumeRecord known_volume;
        known_volume.object_id = volume.object_id;
        known_volume.name = volume.name;
        known_volume.policy_action = volume.policy.action;
        known_volume.policy_summary = volume.policy.summary;
        known_volume.policy_reasons.reserve(volume.policy.reasons.size());
        for (const auto reason : volume.policy.reasons) {
          known_volume.policy_reasons.emplace_back(orchard::apfs::ToString(reason));
        }
        record.volumes.push_back(std::move(known_volume));
      }
    }

    std::sort(record.volumes.begin(), record.volumes.end(),
              [](const KnownVolumeRecord& left, const KnownVolumeRecord& right) {
                return left.object_id < right.object_id;
              });
    return record;
  }
};

class DefaultMountPointAllocator final : public MountPointAllocator {
public:
  [[nodiscard]] blockio::Result<std::wstring>
  Allocate(const std::wstring_view device_path, const KnownVolumeRecord& volume,
           const std::vector<std::wstring>& active_mount_points) override {
    (void)device_path;
    (void)volume;

    std::vector<wchar_t> used_letters;
    used_letters.reserve(active_mount_points.size());
    for (const auto& mount_point : active_mount_points) {
      if (!mount_point.empty()) {
        used_letters.push_back(static_cast<wchar_t>(std::towupper(mount_point.front())));
      }
    }

    const DWORD logical_drives = ::GetLogicalDrives();
    constexpr std::wstring_view kPreferredLetters = L"RSTUVWXYZDEFGHIJKLMNOPQ";
    for (const auto drive_letter : kPreferredLetters) {
      const auto drive_bit = 1UL << (drive_letter - L'A');
      const auto used_by_system = (logical_drives & drive_bit) != 0UL;
      const auto used_by_orchard =
          std::find(used_letters.begin(), used_letters.end(), drive_letter) != used_letters.end();
      if (!used_by_system && !used_by_orchard) {
        return std::wstring{drive_letter, L':'};
      }
    }

    return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                 "No free drive letter is available for automatic Orchard mounts.");
  }
};

KnownDeviceRecord MakeProbeFailureRecord(const DeviceInterfaceInfo& device,
                                         const blockio::Error& error) {
  KnownDeviceRecord record;
  record.device_path = device.device_path;
  record.target_info = blockio::InspectTargetPath(device.device_path);
  record.probe_error = error;
  return record;
}

void CopyRetainedMountBindings(const std::optional<KnownDeviceRecord>& existing_record,
                               KnownDeviceRecord* candidate_record) {
  if (!existing_record.has_value() || candidate_record == nullptr) {
    return;
  }

  for (auto& volume : candidate_record->volumes) {
    const auto existing_volume =
        std::find_if(existing_record->volumes.begin(), existing_record->volumes.end(),
                     [&volume](const KnownVolumeRecord& current) {
                       return current.object_id == volume.object_id;
                     });
    if (existing_volume != existing_record->volumes.end()) {
      volume.mount = existing_volume->mount;
    }
  }
}

std::vector<std::wstring> CollectUnmountsForRecord(const KnownDeviceRecord& record) {
  std::vector<std::wstring> mount_ids;
  for (const auto& volume : record.volumes) {
    if (volume.mount.has_value()) {
      mount_ids.push_back(volume.mount->mount_id);
    }
  }
  return mount_ids;
}

bool ContainsPath(const std::vector<std::wstring>& paths, const std::wstring_view candidate) {
  const auto normalized_candidate = NormalizeDevicePathKey(candidate);
  return std::find_if(paths.begin(), paths.end(), [&](const std::wstring& path) {
           return NormalizeDevicePathKey(path) == normalized_candidate;
         }) != paths.end();
}

} // namespace

DeviceDiscoveryManager::DeviceDiscoveryManager(
    std::unique_ptr<DeviceMonitor> monitor, std::unique_ptr<DeviceEnumerator> enumerator,
    std::unique_ptr<DeviceProber> prober,
    std::unique_ptr<MountPointAllocator> mount_point_allocator, DeviceDiscoveryCallbacks callbacks)
    : monitor_(std::move(monitor)), enumerator_(std::move(enumerator)), prober_(std::move(prober)),
      mount_point_allocator_(std::move(mount_point_allocator)), callbacks_(std::move(callbacks)),
      coordinator_(callbacks_.post_task, [this]() { Reconcile(); }) {}

blockio::Result<std::monostate> DeviceDiscoveryManager::Start() {
  auto start_result =
      monitor_->Start([this](const DeviceMonitorEvent& event) { HandleMonitorEvent(event); });
  if (!start_result.ok()) {
    return start_result.error();
  }

  coordinator_.RequestRescan();
  return std::monostate{};
}

void DeviceDiscoveryManager::Shutdown() noexcept {
  coordinator_.Shutdown();
  monitor_->Stop();
}

std::vector<KnownDeviceRecord> DeviceDiscoveryManager::ListDevices() const {
  return inventory_.ListDevices();
}

void DeviceDiscoveryManager::HandleMonitorEvent(const DeviceMonitorEvent& event) {
  std::scoped_lock lock(event_mutex_);

  switch (event.kind) {
  case DeviceMonitorEventKind::kMountedDeviceQueryRemove:
  case DeviceMonitorEventKind::kMountedDeviceRemovePending:
  case DeviceMonitorEventKind::kMountedDeviceRemoveComplete:
    if (!event.device_path.empty() && !ContainsPath(forced_unmount_paths_, event.device_path)) {
      forced_unmount_paths_.push_back(event.device_path);
    }
    if (!event.device_path.empty() && !ContainsPath(suppressed_mount_paths_, event.device_path)) {
      suppressed_mount_paths_.push_back(event.device_path);
    }
    break;
  case DeviceMonitorEventKind::kInterfaceChange:
    break;
  case DeviceMonitorEventKind::kMountedDeviceQueryRemoveFailed:
    suppressed_mount_paths_.erase(std::remove_if(suppressed_mount_paths_.begin(),
                                                 suppressed_mount_paths_.end(),
                                                 [&event](const std::wstring& path) {
                                                   return NormalizeDevicePathKey(path) ==
                                                          NormalizeDevicePathKey(event.device_path);
                                                 }),
                                  suppressed_mount_paths_.end());
    break;
  }

  coordinator_.RequestRescan();
}

blockio::Result<KnownDeviceRecord>
DeviceDiscoveryManager::ProbeDevice(const DeviceInterfaceInfo& device) const {
  auto probe_result = prober_->Probe(device);
  if (probe_result.ok()) {
    return probe_result.value();
  }
  return MakeProbeFailureRecord(device, probe_result.error());
}

void DeviceDiscoveryManager::ApplyForcedUnmounts() {
  std::vector<std::wstring> pending_paths;
  {
    std::scoped_lock lock(event_mutex_);
    pending_paths = std::move(forced_unmount_paths_);
    forced_unmount_paths_.clear();
  }

  for (const auto& device_path : pending_paths) {
    const auto existing_record = inventory_.FindDevice(device_path);
    if (!existing_record.has_value()) {
      monitor_->UntrackMountedDevice(device_path);
      continue;
    }

    for (const auto& mount_id : CollectUnmountsForRecord(existing_record.value())) {
      const auto unmount_result = callbacks_.unmount_volume(UnmountRequest{.mount_id = mount_id});
      if (unmount_result.ok()) {
        inventory_.DetachMountById(mount_id);
      }
    }

    if (inventory_.MountedVolumeCountForDevice(device_path) == 0U) {
      monitor_->UntrackMountedDevice(device_path);
    }
  }
}

void DeviceDiscoveryManager::RemoveMissingDevices(const DeviceInventoryDiff& diff) {
  for (const auto& device_path : diff.removed_paths) {
    const auto existing_record = inventory_.FindDevice(device_path);
    if (!existing_record.has_value()) {
      continue;
    }

    for (const auto& mount_id : CollectUnmountsForRecord(existing_record.value())) {
      const auto unmount_result = callbacks_.unmount_volume(UnmountRequest{.mount_id = mount_id});
      if (unmount_result.ok()) {
        inventory_.DetachMountById(mount_id);
      }
    }

    monitor_->UntrackMountedDevice(device_path);
    (void)inventory_.RemoveDevice(device_path);
    {
      std::scoped_lock lock(event_mutex_);
      suppressed_mount_paths_.erase(std::remove_if(suppressed_mount_paths_.begin(),
                                                   suppressed_mount_paths_.end(),
                                                   [&device_path](const std::wstring& path) {
                                                     return NormalizeDevicePathKey(path) ==
                                                            NormalizeDevicePathKey(device_path);
                                                   }),
                                    suppressed_mount_paths_.end());
    }
  }
}

bool DeviceDiscoveryManager::ShouldAutoMount(const KnownVolumeRecord& volume) noexcept {
  return volume.policy_action == orchard::apfs::MountDisposition::kMountReadOnly ||
         volume.policy_action == orchard::apfs::MountDisposition::kMountReadWrite;
}

void DeviceDiscoveryManager::ReconcilePresentDevice(const DeviceInterfaceInfo& device) {
  auto existing_record = inventory_.FindDevice(device.device_path);
  auto probe_result = ProbeDevice(device);
  KnownDeviceRecord candidate_record = probe_result.ok()
                                           ? std::move(probe_result.value())
                                           : MakeProbeFailureRecord(device, probe_result.error());

  std::vector<std::wstring> mount_ids_to_remove;
  if (existing_record.has_value()) {
    for (const auto& volume : existing_record->volumes) {
      if (!volume.mount.has_value()) {
        continue;
      }

      const auto matching_candidate =
          std::find_if(candidate_record.volumes.begin(), candidate_record.volumes.end(),
                       [&volume](const KnownVolumeRecord& candidate_volume) {
                         return candidate_volume.object_id == volume.object_id;
                       });
      if (matching_candidate == candidate_record.volumes.end() ||
          !ShouldAutoMount(*matching_candidate)) {
        mount_ids_to_remove.push_back(volume.mount->mount_id);
      }
    }
  }

  for (const auto& mount_id : mount_ids_to_remove) {
    const auto unmount_result = callbacks_.unmount_volume(UnmountRequest{.mount_id = mount_id});
    if (unmount_result.ok()) {
      inventory_.DetachMountById(mount_id);
    }
  }

  existing_record = inventory_.FindDevice(device.device_path);
  CopyRetainedMountBindings(existing_record, &candidate_record);
  inventory_.UpsertDevice(candidate_record);

  auto updated_record = inventory_.FindDevice(device.device_path);
  if (!updated_record.has_value()) {
    return;
  }

  for (const auto& volume : updated_record->volumes) {
    const auto is_suppressed = [&]() {
      std::scoped_lock lock(event_mutex_);
      return ContainsPath(suppressed_mount_paths_, updated_record->device_path);
    }();
    if (is_suppressed || !ShouldAutoMount(volume) || volume.mount.has_value()) {
      continue;
    }

    auto mount_point_result = mount_point_allocator_->Allocate(updated_record->device_path, volume,
                                                               inventory_.ActiveMountPoints());
    if (!mount_point_result.ok()) {
      inventory_.SetMountError(updated_record->device_path, volume.object_id,
                               mount_point_result.error());
      continue;
    }

    auto mount_result = callbacks_.mount_volume(MountRequest{
        .mount_id = std::nullopt,
        .config =
            orchard::fs_winfsp::MountConfig{
                .target_path = updated_record->device_path,
                .mount_point = std::move(mount_point_result.value()),
                .selector =
                    orchard::fs_winfsp::VolumeSelector{
                        .object_id = volume.object_id,
                        .name = std::nullopt,
                    },
                .require_read_only_mount = true,
                .allow_downgrade_from_readwrite = true,
            },
    });
    if (!mount_result.ok()) {
      inventory_.SetMountError(updated_record->device_path, volume.object_id, mount_result.error());
      continue;
    }

    inventory_.AttachMount(updated_record->device_path, volume.object_id,
                           MountedVolumeBinding{
                               .mount_id = mount_result.value().mount_id,
                               .mount_point = mount_result.value().mount_point,
                               .read_only = mount_result.value().read_only,
                           });
  }

  if (inventory_.MountedVolumeCountForDevice(updated_record->device_path) > 0U) {
    const auto track_result = monitor_->TrackMountedDevice(updated_record->device_path);
    (void)track_result;
  } else {
    monitor_->UntrackMountedDevice(updated_record->device_path);
  }
}

void DeviceDiscoveryManager::Reconcile() {
  ApplyForcedUnmounts();

  auto enumerate_result = enumerator_->EnumeratePresentDiskInterfaces();
  if (!enumerate_result.ok()) {
    return;
  }

  const auto diff = inventory_.DiffAgainst(enumerate_result.value());
  RemoveMissingDevices(diff);

  for (const auto& device : enumerate_result.value()) {
    ReconcilePresentDevice(device);
  }
}

std::unique_ptr<DeviceProber> CreateDefaultDeviceProber() {
  return std::make_unique<DefaultDeviceProber>();
}

std::unique_ptr<MountPointAllocator> CreateDefaultMountPointAllocator() {
  return std::make_unique<DefaultMountPointAllocator>();
}

std::unique_ptr<DeviceDiscoveryManager>
CreateDefaultDeviceDiscoveryManager(DeviceDiscoveryCallbacks callbacks) {
  return std::make_unique<DeviceDiscoveryManager>(
      CreateDefaultDeviceMonitor(), CreateDefaultDeviceEnumerator(), CreateDefaultDeviceProber(),
      CreateDefaultMountPointAllocator(), std::move(callbacks));
}

} // namespace orchard::mount_service
