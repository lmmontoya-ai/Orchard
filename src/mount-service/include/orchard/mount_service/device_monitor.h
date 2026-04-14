#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "orchard/blockio/result.h"

namespace orchard::mount_service {

enum class DeviceMonitorEventKind {
  kInterfaceChange,
  kMountedDeviceQueryRemove,
  kMountedDeviceQueryRemoveFailed,
  kMountedDeviceRemovePending,
  kMountedDeviceRemoveComplete,
};

struct DeviceMonitorEvent {
  DeviceMonitorEventKind kind = DeviceMonitorEventKind::kInterfaceChange;
  std::wstring device_path;
};

using DeviceMonitorCallback = std::function<void(const DeviceMonitorEvent&)>;

class DeviceMonitor {
public:
  virtual ~DeviceMonitor() = default;

  [[nodiscard]] virtual blockio::Result<std::monostate> Start(DeviceMonitorCallback callback) = 0;
  virtual void Stop() noexcept = 0;

  [[nodiscard]] virtual blockio::Result<std::monostate>
  TrackMountedDevice(std::wstring_view device_path) = 0;
  virtual void UntrackMountedDevice(std::wstring_view device_path) noexcept = 0;
};

std::unique_ptr<DeviceMonitor> CreateDefaultDeviceMonitor();

} // namespace orchard::mount_service
