#include "orchard/mount_service/runtime.h"

#include <Windows.h>

#include <future>
#include <optional>
#include <utility>

#include "orchard/mount_service/types.h"

namespace orchard::mount_service {
namespace {

constexpr std::uint32_t kDefaultStartWaitHintMs = 3000U;
constexpr std::uint32_t kDefaultStopWaitHintMs = 3000U;

HANDLE AsHandle(void* handle) noexcept {
  return static_cast<HANDLE>(handle);
}

template <typename T> struct AsyncResult {
  std::optional<T> value;
  std::optional<blockio::Error> error;
};

template <typename T> AsyncResult<T> WrapAsyncResult(blockio::Result<T> result) {
  AsyncResult<T> wrapped;
  if (result.ok()) {
    wrapped.value = std::move(result.value());
  } else {
    wrapped.error = result.error();
  }
  return wrapped;
}

template <typename T> blockio::Result<T> UnwrapAsyncResult(AsyncResult<T> wrapped) {
  if (wrapped.value.has_value()) {
    return std::move(*wrapped.value);
  }
  if (wrapped.error.has_value()) {
    return *wrapped.error;
  }
  return MakeMountServiceError(blockio::ErrorCode::kCorruptData,
                               "The Orchard service runtime worker returned no value or error.");
}

} // namespace

ServiceRuntime::ServiceRuntime(ServiceConfig config, std::unique_ptr<MountSessionFactory> factory,
                               ServiceStateCallback state_callback,
                               const bool enable_device_discovery)
    : config_(std::move(config)), registry_(std::move(factory)),
      state_callback_(std::move(state_callback)),
      enable_device_discovery_(enable_device_discovery) {}

ServiceRuntime::~ServiceRuntime() {
  Stop();
  if (stop_event_ != nullptr) {
    ::CloseHandle(AsHandle(stop_event_));
    stop_event_ = nullptr;
  }
}

void ServiceRuntime::EmitState(const ServiceStateSnapshot snapshot) const {
  if (state_callback_) {
    state_callback_(snapshot);
  }
}

blockio::Result<ServiceStateSnapshot>
ServiceRuntime::TransitionState(const ServiceState next_state, const std::uint32_t wait_hint_ms) {
  std::scoped_lock lock(state_mutex_);
  return state_machine_.TransitionTo(next_state, wait_hint_ms);
}

bool ServiceRuntime::IsRunning() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return state_machine_.snapshot().state == ServiceState::kRunning;
}

bool ServiceRuntime::PostCommand(std::function<void()> command) {
  {
    std::scoped_lock lock(queue_mutex_);
    if (worker_exit_requested_) {
      return false;
    }

    commands_.push(std::move(command));
  }

  queue_condition_.notify_one();
  return true;
}

void ServiceRuntime::WorkerLoop() noexcept {
  for (;;) {
    std::function<void()> command;
    {
      std::unique_lock lock(queue_mutex_);
      queue_condition_.wait(
          lock, [this]() noexcept { return worker_exit_requested_ || !commands_.empty(); });

      if (worker_exit_requested_ && commands_.empty()) {
        break;
      }

      command = std::move(commands_.front());
      commands_.pop();
    }

    command();
  }
}

blockio::Result<std::monostate> ServiceRuntime::Start() {
  {
    std::scoped_lock lock(state_mutex_);
    if (state_machine_.snapshot().state != ServiceState::kCreated) {
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "The Orchard service runtime can only be started once.");
    }
  }

  if (stop_event_ == nullptr) {
    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to create the Orchard service stop event.",
                                   ::GetLastError());
    }
  }

  ::ResetEvent(AsHandle(stop_event_));

  auto start_pending_result = TransitionState(ServiceState::kStartPending, kDefaultStartWaitHintMs);
  if (!start_pending_result.ok()) {
    return start_pending_result.error();
  }
  EmitState(start_pending_result.value());

  {
    std::scoped_lock lock(queue_mutex_);
    worker_exit_requested_ = false;
  }

  worker_thread_ = std::thread([this]() noexcept { WorkerLoop(); });

  if (enable_device_discovery_) {
    device_discovery_manager_ = CreateDefaultDeviceDiscoveryManager(DeviceDiscoveryCallbacks{
        .post_task =
            [this](std::function<void()> command) { return PostCommand(std::move(command)); },
        .mount_volume =
            [this](const MountRequest& request) { return registry_.MountVolume(request); },
        .unmount_volume =
            [this](const UnmountRequest& request) { return registry_.UnmountVolume(request); },
    });

    auto discovery_start_result = device_discovery_manager_->Start();
    if (!discovery_start_result.ok()) {
      device_discovery_manager_.reset();
      {
        std::scoped_lock lock(queue_mutex_);
        worker_exit_requested_ = true;
      }
      queue_condition_.notify_all();
      if (worker_thread_.joinable()) {
        worker_thread_.join();
      }
      return discovery_start_result.error();
    }
  }

  auto running_result = TransitionState(ServiceState::kRunning, 0U);
  if (!running_result.ok()) {
    return running_result.error();
  }
  EmitState(running_result.value());

  return std::monostate{};
}

void ServiceRuntime::RequestStop() noexcept {
  std::optional<ServiceStateSnapshot> snapshot_to_emit;

  {
    std::scoped_lock lock(state_mutex_);
    const auto current_state = state_machine_.snapshot().state;
    if (current_state == ServiceState::kRunning || current_state == ServiceState::kStartPending) {
      auto stop_pending_result =
          state_machine_.TransitionTo(ServiceState::kStopPending, kDefaultStopWaitHintMs);
      if (stop_pending_result.ok()) {
        snapshot_to_emit = stop_pending_result.value();
      }
    } else if (current_state == ServiceState::kCreated) {
      auto stopped_result = state_machine_.TransitionTo(ServiceState::kStopped, 0U);
      if (stopped_result.ok()) {
        snapshot_to_emit = stopped_result.value();
      }
    }
  }

  if (snapshot_to_emit.has_value()) {
    EmitState(*snapshot_to_emit);
  }

  if (stop_event_ != nullptr) {
    ::SetEvent(AsHandle(stop_event_));
  }
}

void ServiceRuntime::Stop() noexcept {
  ServiceState current_state;
  {
    std::scoped_lock lock(state_mutex_);
    current_state = state_machine_.snapshot().state;
  }

  if (current_state == ServiceState::kStopped) {
    if (stop_event_ != nullptr) {
      ::SetEvent(AsHandle(stop_event_));
    }
    return;
  }

  RequestStop();

  if (device_discovery_manager_) {
    device_discovery_manager_->Shutdown();
    device_discovery_manager_.reset();
  }

  {
    std::scoped_lock lock(queue_mutex_);
    worker_exit_requested_ = true;
  }
  queue_condition_.notify_all();

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  registry_.Shutdown();

  auto stopped_result = TransitionState(ServiceState::kStopped, 0U);
  if (stopped_result.ok()) {
    EmitState(stopped_result.value());
  }

  if (stop_event_ != nullptr) {
    ::SetEvent(AsHandle(stop_event_));
  }
}

blockio::Result<MountedSessionRecord> ServiceRuntime::MountVolume(const MountRequest& request) {
  if (!IsRunning()) {
    return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                 "Mount requests require a running Orchard service runtime.");
  }

  auto promise = std::make_shared<std::promise<AsyncResult<MountedSessionRecord>>>();
  auto future = promise->get_future();

  {
    if (!PostCommand([this, request, promise]() mutable {
          promise->set_value(WrapAsyncResult(registry_.MountVolume(request)));
        })) {
      return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                   "The Orchard service runtime is stopping.");
    }
  }
  return UnwrapAsyncResult(future.get());
}

blockio::Result<std::monostate> ServiceRuntime::UnmountVolume(const UnmountRequest& request) {
  if (!IsRunning()) {
    return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                 "Unmount requests require a running Orchard service runtime.");
  }

  auto promise = std::make_shared<std::promise<AsyncResult<std::monostate>>>();
  auto future = promise->get_future();

  {
    if (!PostCommand([this, request, promise]() mutable {
          promise->set_value(WrapAsyncResult(registry_.UnmountVolume(request)));
        })) {
      return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                   "The Orchard service runtime is stopping.");
    }
  }
  return UnwrapAsyncResult(future.get());
}

blockio::Result<std::vector<MountedSessionRecord>> ServiceRuntime::ListMounts() {
  if (!IsRunning()) {
    return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                 "List-mounts requests require a running Orchard service runtime.");
  }

  auto promise = std::make_shared<std::promise<AsyncResult<std::vector<MountedSessionRecord>>>>();
  auto future = promise->get_future();

  {
    if (!PostCommand([this, promise]() mutable {
          promise->set_value(WrapAsyncResult(
              blockio::Result<std::vector<MountedSessionRecord>>(registry_.ListMounts())));
        })) {
      return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                   "The Orchard service runtime is stopping.");
    }
  }
  return UnwrapAsyncResult(future.get());
}

blockio::Result<std::vector<KnownDeviceRecord>> ServiceRuntime::ListDevices() {
  if (!IsRunning()) {
    return MakeMountServiceError(
        blockio::ErrorCode::kAccessDenied,
        "List-devices requests require a running Orchard service runtime.");
  }

  auto promise = std::make_shared<std::promise<AsyncResult<std::vector<KnownDeviceRecord>>>>();
  auto future = promise->get_future();

  {
    if (!PostCommand([this, promise]() mutable {
          if (!device_discovery_manager_) {
            promise->set_value(WrapAsyncResult(
                blockio::Result<std::vector<KnownDeviceRecord>>(std::vector<KnownDeviceRecord>{})));
            return;
          }
          promise->set_value(WrapAsyncResult(blockio::Result<std::vector<KnownDeviceRecord>>(
              device_discovery_manager_->ListDevices())));
        })) {
      return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                   "The Orchard service runtime is stopping.");
    }
  }
  return UnwrapAsyncResult(future.get());
}

ServiceStateSnapshot ServiceRuntime::state() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return state_machine_.snapshot();
}

std::uint32_t ServiceRuntime::WaitForStopSignal(const std::uint32_t timeout_ms) const noexcept {
  if (stop_event_ == nullptr) {
    return WAIT_FAILED;
  }

  return ::WaitForSingleObject(AsHandle(stop_event_), timeout_ms);
}

} // namespace orchard::mount_service
