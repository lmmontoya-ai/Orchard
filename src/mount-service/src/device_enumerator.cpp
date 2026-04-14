#include "orchard/mount_service/device_enumerator.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cfgmgr32.h>
#include <ntddstor.h>
#include <winioctl.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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

std::wstring BuildPhysicalDrivePath(const STORAGE_DEVICE_NUMBER& device_number) {
  return LR"(\\.\PhysicalDrive)" + std::to_wstring(device_number.DeviceNumber);
}

blockio::Result<ScopedHandle> OpenInterfaceHandle(std::wstring_view interface_path) {
  const std::wstring interface_path_buffer(interface_path);
  ScopedHandle handle(::CreateFileW(interface_path_buffer.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                 "Failed to open a present disk interface for enumeration.",
                                 ::GetLastError());
  }

  return std::move(handle);
}

blockio::Result<std::wstring> ResolvePhysicalDrivePath(std::wstring_view interface_path) {
  auto handle_result = OpenInterfaceHandle(interface_path);
  if (!handle_result.ok()) {
    return handle_result.error();
  }

  STORAGE_DEVICE_NUMBER device_number{};
  DWORD bytes_returned = 0;
  if (!::DeviceIoControl(handle_result.value().get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                         &device_number, sizeof(device_number), &bytes_returned, nullptr)) {
    return MakeMountServiceError(blockio::ErrorCode::kIoctlFailed,
                                 "Failed to resolve a disk interface to a physical drive number.",
                                 ::GetLastError());
  }

  if (device_number.DeviceType != FILE_DEVICE_DISK) {
    return MakeMountServiceError(blockio::ErrorCode::kUnsupportedTarget,
                                 "The enumerated device interface is not a disk device.");
  }

  return BuildPhysicalDrivePath(device_number);
}

class DefaultDeviceEnumerator final : public DeviceEnumerator {
public:
  [[nodiscard]] blockio::Result<std::vector<DeviceInterfaceInfo>>
  EnumeratePresentDiskInterfaces() override {
    ULONG required_length = 0;
    CONFIGRET config_result = ::CM_Get_Device_Interface_List_SizeW(
        &required_length, const_cast<LPGUID>(&GUID_DEVINTERFACE_DISK), nullptr,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (config_result != CR_SUCCESS) {
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to determine the present disk-interface list length.",
                                   config_result);
    }

    if (required_length == 0U) {
      return std::vector<DeviceInterfaceInfo>{};
    }

    std::vector<wchar_t> buffer(required_length, L'\0');
    config_result = ::CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&GUID_DEVINTERFACE_DISK),
                                                    nullptr, buffer.data(), required_length,
                                                    CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (config_result != CR_SUCCESS) {
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to enumerate present disk interfaces.", config_result);
    }

    std::vector<DeviceInterfaceInfo> devices;
    std::unordered_set<std::wstring> seen_paths;
    for (const wchar_t* cursor = buffer.data(); *cursor != L'\0';
         cursor += std::wcslen(cursor) + 1U) {
      auto physical_drive_result = ResolvePhysicalDrivePath(cursor);
      if (!physical_drive_result.ok()) {
        continue;
      }

      if (seen_paths.insert(physical_drive_result.value()).second) {
        devices.push_back(DeviceInterfaceInfo{
            .device_path = std::move(physical_drive_result.value()),
        });
      }
    }

    std::sort(devices.begin(), devices.end(),
              [](const DeviceInterfaceInfo& left, const DeviceInterfaceInfo& right) {
                return left.device_path < right.device_path;
              });
    return devices;
  }
};

} // namespace

std::unique_ptr<DeviceEnumerator> CreateDefaultDeviceEnumerator() {
  return std::make_unique<DefaultDeviceEnumerator>();
}

} // namespace orchard::mount_service
