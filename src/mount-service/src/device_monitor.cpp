#include "orchard/mount_service/device_monitor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cfgmgr32.h>
#include <ntddstor.h>

#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "orchard/mount_service/types.h"

namespace orchard::mount_service {
namespace {

class ScopedHandle {
public:
  explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  ScopedHandle(ScopedHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle_);
      }
      handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept {
    return handle_;
  }

private:
  HANDLE handle_;
};

std::wstring NormalizeTrackedDevicePath(std::wstring_view device_path) {
  std::wstring normalized(device_path);
  for (auto& ch : normalized) {
    ch = static_cast<wchar_t>(std::towupper(ch));
  }
  return normalized;
}

class DefaultDeviceMonitor final : public DeviceMonitor {
public:
  ~DefaultDeviceMonitor() override {
    Stop();
  }

  [[nodiscard]] blockio::Result<std::monostate> Start(DeviceMonitorCallback callback) override {
    std::scoped_lock lock(mutex_);
    if (running_) {
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "The Orchard device monitor is already running.");
    }

    callback_ = std::move(callback);

    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_DISK;

    const auto config_result = ::CM_Register_Notification(
        &filter, this, &DefaultDeviceMonitor::NotificationThunk, &interface_notification_);
    if (config_result != CR_SUCCESS) {
      callback_ = {};
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to register Orchard for disk-interface notifications.",
                                   config_result);
    }

    running_ = true;
    return std::monostate{};
  }

  void Stop() noexcept override {
    std::unordered_map<std::wstring, HandleRegistration> registrations;
    HCMNOTIFICATION interface_notification = nullptr;

    {
      std::scoped_lock lock(mutex_);
      if (!running_ && interface_notification_ == nullptr && handle_registrations_.empty()) {
        return;
      }

      registrations = std::move(handle_registrations_);
      interface_notification = std::exchange(interface_notification_, nullptr);
      callback_ = {};
      running_ = false;
    }

    for (auto& [device_path, registration] : registrations) {
      (void)device_path;
      if (registration.notification != nullptr) {
        ::CM_Unregister_Notification(registration.notification);
      }
    }

    if (interface_notification != nullptr) {
      ::CM_Unregister_Notification(interface_notification);
    }
  }

  [[nodiscard]] blockio::Result<std::monostate>
  TrackMountedDevice(const std::wstring_view device_path) override {
    const auto normalized_path = NormalizeTrackedDevicePath(device_path);
    const std::wstring device_path_buffer(device_path);

    {
      std::scoped_lock lock(mutex_);
      if (handle_registrations_.contains(normalized_path)) {
        return std::monostate{};
      }
    }

    ScopedHandle handle(::CreateFileW(device_path_buffer.c_str(), 0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
      return MakeMountServiceError(
          blockio::ErrorCode::kOpenFailed,
          "Failed to open a mounted Orchard device for removal notifications.", ::GetLastError());
    }

    auto context = std::make_unique<HandleRegistrationContext>();
    context->owner = this;
    context->device_path = device_path_buffer;

    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    filter.u.DeviceHandle.hTarget = handle.get();

    HCMNOTIFICATION notification = nullptr;
    const auto config_result = ::CM_Register_Notification(
        &filter, context.get(), &DefaultDeviceMonitor::NotificationThunk, &notification);
    if (config_result != CR_SUCCESS) {
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to register Orchard for mounted-device notifications.",
                                   config_result);
    }

    std::scoped_lock lock(mutex_);
    handle_registrations_.emplace(normalized_path, HandleRegistration{
                                                       .context = std::move(context),
                                                       .handle = std::move(handle),
                                                       .notification = notification,
                                                   });
    return std::monostate{};
  }

  void UntrackMountedDevice(const std::wstring_view device_path) noexcept override {
    HandleRegistration registration;
    bool found = false;

    {
      std::scoped_lock lock(mutex_);
      const auto normalized_path = NormalizeTrackedDevicePath(device_path);
      const auto existing = handle_registrations_.find(normalized_path);
      if (existing == handle_registrations_.end()) {
        return;
      }

      registration = std::move(existing->second);
      handle_registrations_.erase(existing);
      found = true;
    }

    if (found && registration.notification != nullptr) {
      ::CM_Unregister_Notification(registration.notification);
    }
  }

private:
  struct HandleRegistrationContext {
    DefaultDeviceMonitor* owner = nullptr;
    std::wstring device_path;
  };

  struct HandleRegistration {
    std::unique_ptr<HandleRegistrationContext> context;
    ScopedHandle handle;
    HCMNOTIFICATION notification = nullptr;
  };

  static DWORD CALLBACK NotificationThunk(HCMNOTIFICATION notify, PVOID context,
                                          CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA event_data,
                                          DWORD event_data_size) {
    (void)notify;
    (void)event_data_size;
    if (event_data == nullptr) {
      return ERROR_SUCCESS;
    }

    if (event_data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) {
      auto* self = static_cast<DefaultDeviceMonitor*>(context);
      if (self != nullptr) {
        self->Emit(DeviceMonitorEvent{
            .kind = DeviceMonitorEventKind::kInterfaceChange,
            .device_path = event_data->u.DeviceInterface.SymbolicLink,
        });
      }
      return ERROR_SUCCESS;
    }

    auto* registration_context = static_cast<HandleRegistrationContext*>(context);
    if (registration_context == nullptr || registration_context->owner == nullptr) {
      return ERROR_SUCCESS;
    }

    auto* self = registration_context->owner;
    switch (action) {
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
      self->Emit(DeviceMonitorEvent{
          .kind = DeviceMonitorEventKind::kMountedDeviceQueryRemove,
          .device_path = registration_context->device_path,
      });
      break;
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
      self->Emit(DeviceMonitorEvent{
          .kind = DeviceMonitorEventKind::kMountedDeviceQueryRemoveFailed,
          .device_path = registration_context->device_path,
      });
      break;
    case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
      self->Emit(DeviceMonitorEvent{
          .kind = DeviceMonitorEventKind::kMountedDeviceRemovePending,
          .device_path = registration_context->device_path,
      });
      break;
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
      self->Emit(DeviceMonitorEvent{
          .kind = DeviceMonitorEventKind::kMountedDeviceRemoveComplete,
          .device_path = registration_context->device_path,
      });
      break;
    default:
      break;
    }

    return ERROR_SUCCESS;
  }

  void Emit(const DeviceMonitorEvent& event) {
    DeviceMonitorCallback callback;
    {
      std::scoped_lock lock(mutex_);
      callback = callback_;
    }

    if (callback) {
      callback(event);
    }
  }

  std::mutex mutex_;
  DeviceMonitorCallback callback_;
  HCMNOTIFICATION interface_notification_ = nullptr;
  std::unordered_map<std::wstring, HandleRegistration> handle_registrations_;
  bool running_ = false;
};

} // namespace

std::unique_ptr<DeviceMonitor> CreateDefaultDeviceMonitor() {
  return std::make_unique<DefaultDeviceMonitor>();
}

} // namespace orchard::mount_service
