#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <variant>
#include <vector>

#include "orchard/blockio/result.h"
#include "orchard/mount_service/device_enumerator.h"
#include "orchard/mount_service/device_inventory.h"
#include "orchard/mount_service/device_monitor.h"
#include "orchard/mount_service/rescan_coordinator.h"
#include "orchard/mount_service/types.h"

namespace orchard::mount_service {

class DeviceProber {
public:
  virtual ~DeviceProber() = default;

  [[nodiscard]] virtual blockio::Result<KnownDeviceRecord>
  Probe(const DeviceInterfaceInfo& device) = 0;
};

class MountPointAllocator {
public:
  virtual ~MountPointAllocator() = default;

  [[nodiscard]] virtual blockio::Result<std::wstring>
  Allocate(std::wstring_view device_path, const KnownVolumeRecord& volume,
           const std::vector<std::wstring>& active_mount_points) = 0;
};

using MountVolumeCallback =
    std::function<blockio::Result<MountedSessionRecord>(const MountRequest&)>;
using UnmountVolumeCallback = std::function<blockio::Result<std::monostate>(const UnmountRequest&)>;
using ListMountsCallback = std::function<blockio::Result<std::vector<MountedSessionRecord>>()>;

struct DeviceDiscoveryCallbacks {
  RescanTaskPoster post_task;
  MountVolumeCallback mount_volume;
  UnmountVolumeCallback unmount_volume;
  ListMountsCallback list_mounts;
};

class DeviceDiscoveryManager {
public:
  DeviceDiscoveryManager(std::unique_ptr<DeviceMonitor> monitor,
                         std::unique_ptr<DeviceEnumerator> enumerator,
                         std::unique_ptr<DeviceProber> prober,
                         std::unique_ptr<MountPointAllocator> mount_point_allocator,
                         DeviceDiscoveryCallbacks callbacks);

  [[nodiscard]] blockio::Result<std::monostate> Start();
  void Shutdown() noexcept;

  [[nodiscard]] std::vector<KnownDeviceRecord> ListDevices() const;

private:
  void HandleMonitorEvent(const DeviceMonitorEvent& event);
  void Reconcile();

  [[nodiscard]] blockio::Result<KnownDeviceRecord>
  ProbeDevice(const DeviceInterfaceInfo& device) const;
  void ApplyForcedUnmounts();
  void RemoveMissingDevices(const DeviceInventoryDiff& diff);
  void ReconcilePresentDevice(const DeviceInterfaceInfo& device);

  [[nodiscard]] static bool ShouldAutoMount(const KnownVolumeRecord& volume) noexcept;

  std::unique_ptr<DeviceMonitor> monitor_;
  std::unique_ptr<DeviceEnumerator> enumerator_;
  std::unique_ptr<DeviceProber> prober_;
  std::unique_ptr<MountPointAllocator> mount_point_allocator_;
  DeviceDiscoveryCallbacks callbacks_;
  RescanCoordinator coordinator_;
  DeviceInventory inventory_;
  mutable std::mutex event_mutex_;
  std::vector<std::wstring> forced_unmount_paths_;
  std::vector<std::wstring> suppressed_mount_paths_;
};

std::unique_ptr<DeviceProber> CreateDefaultDeviceProber();
std::unique_ptr<MountPointAllocator> CreateDefaultMountPointAllocator();
std::unique_ptr<DeviceDiscoveryManager>
CreateDefaultDeviceDiscoveryManager(DeviceDiscoveryCallbacks callbacks);

} // namespace orchard::mount_service
