#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orchard/fs_winfsp/mount.h"
#include "orchard/fs_winfsp/path_bridge.h"

namespace {

HANDLE g_shutdown_event = nullptr;
constexpr DWORD kMountVisibilityTimeoutMs = 5000;

struct MountPointProbe {
  bool dos_device_present = false;
  DWORD dos_device_error = ERROR_SUCCESS;
  std::wstring dos_device_target;
  UINT drive_type = DRIVE_NO_ROOT_DIR;
  DWORD root_attributes = INVALID_FILE_ATTRIBUTES;
  DWORD root_error = ERROR_SUCCESS;
};

BOOL WINAPI ConsoleControlHandler(const DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    if (g_shutdown_event != nullptr) {
      ::SetEvent(g_shutdown_event);
    }
    return TRUE;
  }

  return FALSE;
}

void PrintUsage() {
  std::cerr << "Usage: orchard-mount-smoke --target <path> --mountpoint <X:|dir> "
               "[--volume-name <name>] [--volume-oid <id>] [--hold-ms <milliseconds>] "
               "[--shutdown-event <name>]\n";
}

std::optional<std::uint64_t> ParseUint64(const std::string_view text) {
  try {
    std::size_t parsed = 0;
    const auto value = std::stoull(std::string(text), &parsed, 0);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::wstring NormalizeMountRoot(std::wstring mount_point) {
  std::replace(mount_point.begin(), mount_point.end(), L'/', L'\\');
  if (mount_point.size() == 2U && mount_point[1] == L':') {
    mount_point.push_back(L'\\');
  }
  return mount_point;
}

bool IsDriveLetterMountPoint(const std::wstring_view mount_point) noexcept {
  return mount_point.size() == 2U && mount_point[1] == L':';
}

MountPointProbe ProbeMountPoint(const std::wstring_view mount_point) {
  MountPointProbe probe;
  const auto root = NormalizeMountRoot(std::wstring(mount_point));

  if (IsDriveLetterMountPoint(mount_point)) {
    wchar_t buffer[1024]{};
    if (::QueryDosDeviceW(std::wstring(mount_point).c_str(), buffer,
                          static_cast<DWORD>(std::size(buffer))) != 0U) {
      probe.dos_device_present = true;
      probe.dos_device_target = buffer;
    } else {
      probe.dos_device_error = ::GetLastError();
    }
  }

  probe.drive_type = ::GetDriveTypeW(root.c_str());
  probe.root_attributes = ::GetFileAttributesW(root.c_str());
  if (probe.root_attributes == INVALID_FILE_ATTRIBUTES) {
    probe.root_error = ::GetLastError();
  }
  return probe;
}

bool IsMountPointAccessible(const MountPointProbe& probe) noexcept {
  return probe.root_attributes != INVALID_FILE_ATTRIBUTES;
}

void PrintMountPointProbe(const std::wstring_view mount_point, const MountPointProbe& probe) {
  std::wcerr << L"Mount-point diagnostics for '" << mount_point << L"':\n";
  if (IsDriveLetterMountPoint(mount_point)) {
    if (probe.dos_device_present) {
      std::wcerr << L"  QueryDosDevice: present -> " << probe.dos_device_target << L"\n";
    } else {
      std::wcerr << L"  QueryDosDevice: missing, win32=" << probe.dos_device_error << L"\n";
    }
  }
  std::wcerr << L"  GetDriveType: " << probe.drive_type << L"\n";
  if (probe.root_attributes != INVALID_FILE_ATTRIBUTES) {
    std::wcerr << L"  GetFileAttributes: success, attributes=0x" << std::hex
               << probe.root_attributes << std::dec << L"\n";
  } else {
    std::wcerr << L"  GetFileAttributes: failed, win32=" << probe.root_error << L"\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  orchard::fs_winfsp::MountConfig config;
  std::optional<DWORD> hold_timeout_ms;
  std::optional<std::wstring> shutdown_event_name;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if ((argument == "--target" || argument == "-t") && index + 1 < argc) {
      config.target_path = argv[++index];
      continue;
    }
    if ((argument == "--mountpoint" || argument == "-m") && index + 1 < argc) {
      const auto wide_result = orchard::fs_winfsp::Utf8ToWide(argv[index + 1]);
      if (!wide_result.ok()) {
        std::cerr << "Invalid UTF-8 mount point.\n";
        return 1;
      }
      config.mount_point = wide_result.value();
      ++index;
      continue;
    }
    if (argument == "--volume-name" && index + 1 < argc) {
      config.selector.name = std::string(argv[++index]);
      continue;
    }
    if (argument == "--volume-oid" && index + 1 < argc) {
      auto value = ParseUint64(argv[++index]);
      if (!value.has_value()) {
        std::cerr << "Invalid --volume-oid value.\n";
        return 1;
      }
      config.selector.object_id = value;
      continue;
    }
    if (argument == "--hold-ms" && index + 1 < argc) {
      auto value = ParseUint64(argv[++index]);
      if (!value.has_value() || *value > static_cast<std::uint64_t>(MAXDWORD)) {
        std::cerr << "Invalid --hold-ms value.\n";
        return 1;
      }
      hold_timeout_ms = static_cast<DWORD>(*value);
      continue;
    }
    if (argument == "--shutdown-event" && index + 1 < argc) {
      const auto wide_result = orchard::fs_winfsp::Utf8ToWide(argv[index + 1]);
      if (!wide_result.ok()) {
        std::cerr << "Invalid UTF-8 shutdown event name.\n";
        return 1;
      }
      shutdown_event_name = wide_result.value();
      ++index;
      continue;
    }

    PrintUsage();
    return 1;
  }

  if (config.target_path.empty() || config.mount_point.empty()) {
    PrintUsage();
    return 1;
  }

  auto mount_result = orchard::fs_winfsp::StartMount(config);
  if (!mount_result.ok()) {
    std::cerr << "Mount failed: " << mount_result.error().message << "\n";
    std::cerr << "WinFsp support: " << orchard::fs_winfsp::WinFspSupportStatus() << "\n";
    return 1;
  }

  MountPointProbe probe;
  const auto deadline = ::GetTickCount64() + kMountVisibilityTimeoutMs;
  do {
    probe = ProbeMountPoint(config.mount_point);
    if (IsMountPointAccessible(probe)) {
      break;
    }
    ::Sleep(100);
  } while (::GetTickCount64() < deadline);

  if (!IsMountPointAccessible(probe)) {
    std::wcerr << L"Mount session started, but the mount point did not become reachable within "
               << kMountVisibilityTimeoutMs << L" ms.\n";
    PrintMountPointProbe(config.mount_point, probe);
    mount_result.value()->Stop();
    return 1;
  }

  std::wcout << L"Mounted '" << mount_result.value()->mounted_volume().volume_label() << L"' at "
             << mount_result.value()->mount_point() << L". Press Ctrl-C to unmount.\n";
  if (IsDriveLetterMountPoint(config.mount_point)) {
    std::wcout << L"Note: drive-letter smoke mounts created from an interactive process may be "
                  L"visible only to the current logon token. Use the Orchard service path to "
                  L"validate normal File Explorer visibility.\n";
  }

  g_shutdown_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_shutdown_event == nullptr) {
    std::cerr << "Failed to create the shutdown event.\n";
    mount_result.value()->Stop();
    return 1;
  }

  HANDLE external_shutdown_event = nullptr;
  if (shutdown_event_name.has_value()) {
    external_shutdown_event = ::CreateEventW(nullptr, TRUE, FALSE, shutdown_event_name->c_str());
    if (external_shutdown_event == nullptr) {
      std::cerr << "Failed to create or open the external shutdown event.\n";
      mount_result.value()->Stop();
      return 1;
    }
  }

  ::SetConsoleCtrlHandler(&ConsoleControlHandler, TRUE);
  std::vector<HANDLE> wait_handles{g_shutdown_event};
  if (external_shutdown_event != nullptr) {
    wait_handles.push_back(external_shutdown_event);
  }

  const auto wait_result =
      ::WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()), wait_handles.data(), FALSE,
                               hold_timeout_ms.value_or(INFINITE));
  ::SetConsoleCtrlHandler(&ConsoleControlHandler, FALSE);
  if (external_shutdown_event != nullptr) {
    ::CloseHandle(external_shutdown_event);
  }
  ::CloseHandle(g_shutdown_event);
  g_shutdown_event = nullptr;

  if (wait_result == WAIT_FAILED) {
    std::cerr << "Failed while waiting for shutdown.\n";
    mount_result.value()->Stop();
    return 1;
  }

  mount_result.value()->Stop();
  return 0;
}
