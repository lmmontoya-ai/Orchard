#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "orchard/blockio/result.h"
#include "orchard/mount_service/device_discovery.h"
#include "orchard/mount_service/mount_registry.h"
#include "orchard/mount_service/service_state.h"
#include "orchard/mount_service/types.h"

namespace orchard::mount_service {

using ServiceStateCallback = std::function<void(const ServiceStateSnapshot&)>;

class ServiceRuntime {
public:
  explicit ServiceRuntime(
      ServiceConfig config = {},
      std::unique_ptr<MountSessionFactory> factory = CreateDefaultMountSessionFactory(),
      ServiceStateCallback state_callback = {}, bool enable_device_discovery = true);
  ~ServiceRuntime();

  ServiceRuntime(const ServiceRuntime&) = delete;
  ServiceRuntime& operator=(const ServiceRuntime&) = delete;

  [[nodiscard]] blockio::Result<std::monostate> Start();
  void RequestStop() noexcept;
  void Stop() noexcept;

  [[nodiscard]] blockio::Result<MountedSessionRecord> MountVolume(const MountRequest& request);
  [[nodiscard]] blockio::Result<std::monostate> UnmountVolume(const UnmountRequest& request);
  [[nodiscard]] blockio::Result<std::vector<MountedSessionRecord>> ListMounts();
  [[nodiscard]] blockio::Result<std::vector<KnownDeviceRecord>> ListDevices();

  [[nodiscard]] ServiceStateSnapshot state() const noexcept;
  [[nodiscard]] std::wstring_view service_name() const noexcept {
    return config_.service_name;
  }
  [[nodiscard]] std::uint32_t WaitForStopSignal(std::uint32_t timeout_ms) const noexcept;

private:
  [[nodiscard]] bool PostCommand(std::function<void()> command);
  void WorkerLoop() noexcept;
  void EmitState(ServiceStateSnapshot snapshot) const;
  [[nodiscard]] blockio::Result<ServiceStateSnapshot> TransitionState(ServiceState next_state,
                                                                      std::uint32_t wait_hint_ms);
  [[nodiscard]] bool IsRunning() const noexcept;
  [[nodiscard]] bool HasStarted() const noexcept;

  ServiceConfig config_;
  MountRegistry registry_;
  ServiceStateMachine state_machine_;
  ServiceStateCallback state_callback_;
  bool enable_device_discovery_ = true;
  std::unique_ptr<DeviceDiscoveryManager> device_discovery_manager_;

  void* stop_event_ = nullptr;

  mutable std::mutex state_mutex_;
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::queue<std::function<void()>> commands_;
  std::thread worker_thread_;
  bool worker_exit_requested_ = false;
};

} // namespace orchard::mount_service
