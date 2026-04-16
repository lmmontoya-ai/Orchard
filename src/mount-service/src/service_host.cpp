#include "orchard/mount_service/service_host.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsvc.h>

#include <algorithm>
#include <cwctype>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "orchard/blockio/error.h"
#include "orchard/fs_winfsp/path_bridge.h"
#include "orchard/mount_service/runtime.h"

namespace orchard::mount_service {
namespace {

constexpr std::uint32_t kConsoleWaitSliceMs = 250U;

ServiceRuntime* g_console_runtime = nullptr;

BOOL WINAPI ConsoleControlHandler(const DWORD control_type) {
  if ((control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
       control_type == CTRL_CLOSE_EVENT) &&
      g_console_runtime != nullptr) {
    g_console_runtime->RequestStop();
    return TRUE;
  }

  return FALSE;
}

std::wstring NormalizeWideKey(std::wstring_view value) {
  std::wstring normalized(value);
  for (auto& ch : normalized) {
    ch = static_cast<wchar_t>(::towupper(ch));
  }
  return normalized;
}

class ScopedScHandle {
public:
  explicit ScopedScHandle(SC_HANDLE handle = nullptr) noexcept : handle_(handle) {}
  ~ScopedScHandle() {
    if (handle_ != nullptr) {
      ::CloseServiceHandle(handle_);
    }
  }

  ScopedScHandle(const ScopedScHandle&) = delete;
  ScopedScHandle& operator=(const ScopedScHandle&) = delete;

  ScopedScHandle(ScopedScHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  ScopedScHandle& operator=(ScopedScHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        ::CloseServiceHandle(handle_);
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] SC_HANDLE get() const noexcept {
    return handle_;
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

private:
  SC_HANDLE handle_;
};

struct ParseWideArgumentRequest {
  std::string_view text;
  std::string_view name;
};

struct WaitForServiceStateRequest {
  DWORD expected_state = SERVICE_STOPPED;
  DWORD timeout_ms = 0;
};

[[nodiscard]] blockio::Result<std::uint64_t> ParseUint64(const std::string_view text) {
  try {
    std::size_t parsed = 0;
    const auto value = std::stoull(std::string(text), &parsed, 0);
    if (parsed != text.size()) {
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "Expected an unsigned integer argument.");
    }
    return value;
  } catch (...) {
    return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                 "Expected an unsigned integer argument.");
  }
}

[[nodiscard]] blockio::Result<std::wstring>
ParseWideArgument(const ParseWideArgumentRequest& request) {
  auto wide_result = orchard::fs_winfsp::Utf8ToWide(request.text);
  if (!wide_result.ok()) {
    return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                 "Invalid UTF-8 provided for " + std::string(request.name) + ".");
  }
  return wide_result.value();
}

[[nodiscard]] blockio::Result<std::wstring> CurrentExecutablePath() {
  std::wstring path(MAX_PATH, L'\0');
  const auto length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0U) {
    return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                 "Failed to query the Orchard service-host executable path.",
                                 ::GetLastError());
  }

  path.resize(length);
  return path;
}

void AppendCommandLineArgument(std::wstring& command_line, const std::wstring_view argument) {
  command_line += L" \"";
  command_line += argument;
  command_line += L"\"";
}

[[nodiscard]] blockio::Result<std::wstring>
BuildServiceBinaryPath(const ServiceHostOptions& options) {
  auto executable_path_result = CurrentExecutablePath();
  if (!executable_path_result.ok()) {
    return executable_path_result.error();
  }

  std::wstring command_line = L"\"" + executable_path_result.value() + L"\"";
  if (options.service.service_name != kDefaultServiceName) {
    command_line += L" --service-name";
    AppendCommandLineArgument(command_line, options.service.service_name);
  }

  if (options.startup_mount.has_value()) {
    command_line += L" --target";
    AppendCommandLineArgument(command_line, options.startup_mount->config.target_path.wstring());
    command_line += L" --mountpoint";
    AppendCommandLineArgument(command_line, options.startup_mount->config.mount_point);

    if (options.startup_mount->config.selector.name.has_value()) {
      auto wide_name_result =
          orchard::fs_winfsp::Utf8ToWide(*options.startup_mount->config.selector.name);
      if (!wide_name_result.ok()) {
        return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                     "Invalid UTF-8 provided for --volume-name.");
      }
      command_line += L" --volume-name";
      AppendCommandLineArgument(command_line, wide_name_result.value());
    }

    if (options.startup_mount->config.selector.object_id.has_value()) {
      command_line += L" --volume-oid";
      AppendCommandLineArgument(command_line,
                                std::to_wstring(*options.startup_mount->config.selector.object_id));
    }
  }
  return command_line;
}

[[nodiscard]] blockio::Result<std::monostate>
InstallServiceBinary(const ServiceHostOptions& options) {
  ScopedScHandle service_manager(
      ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (!service_manager) {
    return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                 "Failed to open the Windows Service Control Manager for install.",
                                 ::GetLastError());
  }

  auto binary_path_result = BuildServiceBinaryPath(options);
  if (!binary_path_result.ok()) {
    return binary_path_result.error();
  }

  ScopedScHandle service(::CreateServiceW(
      service_manager.get(), options.service.service_name.c_str(),
      options.service.display_name.c_str(), SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
      SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binary_path_result.value().c_str(), nullptr,
      nullptr, nullptr, nullptr, nullptr));
  if (!service) {
    return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                 "Failed to create the Orchard Windows service.", ::GetLastError());
  }

  SERVICE_DESCRIPTIONW description{
      .lpDescription = const_cast<LPWSTR>(options.service.description.c_str()),
  };
  if (!::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description)) {
    return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                 "Failed to apply the Orchard service description.",
                                 ::GetLastError());
  }

  return std::monostate{};
}

[[nodiscard]] blockio::Result<std::monostate>
WaitForServiceState(const SC_HANDLE service, const WaitForServiceStateRequest& request) {
  const auto deadline = ::GetTickCount64() + request.timeout_ms;
  SERVICE_STATUS_PROCESS status{};
  DWORD bytes_needed = 0;

  while (::GetTickCount64() <= deadline) {
    if (!::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
                                sizeof(status), &bytes_needed)) {
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to query Orchard service status.", ::GetLastError());
    }

    if (status.dwCurrentState == request.expected_state) {
      return std::monostate{};
    }

    ::Sleep(250);
  }

  return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                               "Timed out waiting for the Orchard service state transition.");
}

[[nodiscard]] blockio::Result<std::monostate>
MaybeMountStartupVolume(ServiceRuntime& runtime, const ServiceHostOptions& options);

bool MountMatchesStartupRequest(const MountedSessionRecord& mount, const MountRequest& request) {
  if (NormalizeWideKey(mount.mount_point) != NormalizeWideKey(request.config.mount_point)) {
    return false;
  }

  if (NormalizeWideKey(mount.target_path.wstring()) !=
      NormalizeWideKey(request.config.target_path.wstring())) {
    return false;
  }

  if (request.config.selector.object_id.has_value() &&
      mount.volume_object_id != *request.config.selector.object_id) {
    return false;
  }

  if (request.config.selector.name.has_value() &&
      mount.volume_name != *request.config.selector.name) {
    return false;
  }

  return true;
}

[[nodiscard]] blockio::Result<std::optional<MountedSessionRecord>>
FindMatchingStartupMount(ServiceRuntime& runtime, const MountRequest& request) {
  auto mounts_result = runtime.ListMounts();
  if (!mounts_result.ok()) {
    return mounts_result.error();
  }

  const auto match = std::find_if(mounts_result.value().begin(), mounts_result.value().end(),
                                  [&request](const MountedSessionRecord& mount) {
                                    return MountMatchesStartupRequest(mount, request);
                                  });
  if (match == mounts_result.value().end()) {
    return std::optional<MountedSessionRecord>{};
  }

  return std::optional<MountedSessionRecord>{*match};
}

[[nodiscard]] blockio::Result<std::monostate> UninstallServiceBinary(const ServiceConfig& config) {
  ScopedScHandle service_manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!service_manager) {
    return MakeMountServiceError(
        blockio::ErrorCode::kAccessDenied,
        "Failed to open the Windows Service Control Manager for uninstall.", ::GetLastError());
  }

  ScopedScHandle service(::OpenServiceW(service_manager.get(), config.service_name.c_str(),
                                        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
  if (!service) {
    return MakeMountServiceError(blockio::ErrorCode::kNotFound,
                                 "The Orchard Windows service is not installed.", ::GetLastError());
  }

  SERVICE_STATUS status{};
  if (::ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
    auto stopped_result = WaitForServiceState(service.get(), WaitForServiceStateRequest{
                                                                 .expected_state = SERVICE_STOPPED,
                                                                 .timeout_ms = 15000U,
                                                             });
    if (!stopped_result.ok()) {
      return stopped_result.error();
    }
  }

  if (!::DeleteService(service.get())) {
    return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                 "Failed to delete the Orchard Windows service.", ::GetLastError());
  }

  return std::monostate{};
}

class WindowsServiceHost {
public:
  explicit WindowsServiceHost(ServiceHostOptions options) : options_(std::move(options)) {}

  int RunDispatcher() {
    active_instance_ = this;

    SERVICE_TABLE_ENTRYW service_table[] = {
        {
            .lpServiceName = options_.service.service_name.data(),
            .lpServiceProc = &WindowsServiceHost::ServiceMainThunk,
        },
        {
            .lpServiceName = nullptr,
            .lpServiceProc = nullptr,
        },
    };

    if (!::StartServiceCtrlDispatcherW(service_table)) {
      if (::GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        std::cerr << "orchard-service-host must be run with --console when started manually.\n";
      }

      active_instance_ = nullptr;
      return 1;
    }

    active_instance_ = nullptr;
    return exit_code_;
  }

private:
  struct ServiceControlRequest {
    DWORD control = 0;
    DWORD event_type = 0;
    void* event_data = nullptr;
  };

  static void WINAPI ServiceMainThunk(const DWORD argc, LPWSTR* argv) {
    if (active_instance_ != nullptr) {
      active_instance_->ServiceMain(argc, argv);
    }
  }

  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  static DWORD WINAPI ControlHandlerThunk(const DWORD control, const DWORD event_type,
                                          LPVOID event_data, LPVOID context) {
    auto* self = static_cast<WindowsServiceHost*>(context);
    return self->HandleControl(ServiceControlRequest{
        .control = control,
        .event_type = event_type,
        .event_data = event_data,
    });
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

  void ServiceMain(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;

    status_handle_ = ::RegisterServiceCtrlHandlerExW(
        options_.service.service_name.c_str(), &WindowsServiceHost::ControlHandlerThunk, this);
    if (status_handle_ == nullptr) {
      exit_code_ = 1;
      return;
    }

    runtime_ = std::make_unique<ServiceRuntime>(
        options_.service, CreateDefaultMountSessionFactory(),
        [this](const ServiceStateSnapshot& snapshot) { ReportState(snapshot, NO_ERROR); });

    auto start_result = runtime_->Start();
    if (!start_result.ok()) {
      ReportState(ServiceStateSnapshot{.state = ServiceState::kStopped},
                  ERROR_SERVICE_SPECIFIC_ERROR);
      exit_code_ = 1;
      return;
    }

    auto mount_result = MaybeMountStartupVolume(*runtime_, options_);
    if (!mount_result.ok()) {
      runtime_->Stop();
      ReportState(ServiceStateSnapshot{.state = ServiceState::kStopped},
                  ERROR_SERVICE_SPECIFIC_ERROR);
      exit_code_ = 1;
      return;
    }

    const auto wait_result = runtime_->WaitForStopSignal(INFINITE);
    (void)wait_result;
    runtime_->Stop();
    exit_code_ = 0;
  }

  DWORD HandleControl(const ServiceControlRequest& request) {
    (void)request.event_type;
    (void)request.event_data;

    switch (request.control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      if (runtime_) {
        runtime_->RequestStop();
      }
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      if (runtime_) {
        ReportState(runtime_->state(), NO_ERROR);
      }
      return NO_ERROR;
    default:
      return NO_ERROR;
    }
  }

  void ReportState(const ServiceStateSnapshot& snapshot, const DWORD win32_exit_code) {
    if (status_handle_ == nullptr) {
      return;
    }

    SERVICE_STATUS status{
        .dwServiceType = SERVICE_WIN32_OWN_PROCESS,
        .dwCurrentState = SERVICE_STOPPED,
        .dwControlsAccepted = 0,
        .dwWin32ExitCode = win32_exit_code,
        .dwServiceSpecificExitCode = 0,
        .dwCheckPoint = snapshot.checkpoint,
        .dwWaitHint = snapshot.wait_hint_ms,
    };

    switch (snapshot.state) {
    case ServiceState::kCreated:
    case ServiceState::kStartPending:
      status.dwCurrentState = SERVICE_START_PENDING;
      break;
    case ServiceState::kRunning:
      status.dwCurrentState = SERVICE_RUNNING;
      status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
      break;
    case ServiceState::kStopPending:
      status.dwCurrentState = SERVICE_STOP_PENDING;
      break;
    case ServiceState::kStopped:
      status.dwCurrentState = SERVICE_STOPPED;
      break;
    }

    ::SetServiceStatus(status_handle_, &status);
  }

  inline static WindowsServiceHost* active_instance_ = nullptr;

  ServiceHostOptions options_;
  SERVICE_STATUS_HANDLE status_handle_ = nullptr;
  std::unique_ptr<ServiceRuntime> runtime_;
  int exit_code_ = 0;
};

[[nodiscard]] blockio::Result<std::monostate>
MaybeMountStartupVolume(ServiceRuntime& runtime, const ServiceHostOptions& options) {
  if (!options.startup_mount.has_value()) {
    return std::monostate{};
  }

  auto existing_mount_result = FindMatchingStartupMount(runtime, *options.startup_mount);
  if (!existing_mount_result.ok()) {
    return existing_mount_result.error();
  }
  if (existing_mount_result.value().has_value()) {
    return std::monostate{};
  }

  auto mount_result = runtime.MountVolume(*options.startup_mount);
  if (!mount_result.ok()) {
    existing_mount_result = FindMatchingStartupMount(runtime, *options.startup_mount);
    if (!existing_mount_result.ok()) {
      return existing_mount_result.error();
    }
    if (existing_mount_result.value().has_value()) {
      return std::monostate{};
    }
    return mount_result.error();
  }

  std::wcout << L"Mounted '" << mount_result.value().volume_label << L"' at "
             << mount_result.value().mount_point << L" with mount ID '"
             << mount_result.value().mount_id << L"'.\n";
  return std::monostate{};
}

[[nodiscard]] blockio::Result<std::monostate>
WaitForConsoleShutdown(ServiceRuntime& runtime, const ServiceHostOptions& options) {
  HANDLE external_shutdown_event = nullptr;
  if (options.shutdown_event_name.has_value()) {
    external_shutdown_event =
        ::CreateEventW(nullptr, TRUE, FALSE, options.shutdown_event_name->c_str());
    if (external_shutdown_event == nullptr) {
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed to create the Orchard service-host shutdown event.",
                                   ::GetLastError());
    }
  }

  const auto started_at = ::GetTickCount64();
  for (;;) {
    const auto wait_result = runtime.WaitForStopSignal(kConsoleWaitSliceMs);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (wait_result == WAIT_FAILED) {
      if (external_shutdown_event != nullptr) {
        ::CloseHandle(external_shutdown_event);
      }
      return MakeMountServiceError(blockio::ErrorCode::kOpenFailed,
                                   "Failed while waiting for Orchard console shutdown.",
                                   ::GetLastError());
    }

    if (external_shutdown_event != nullptr &&
        ::WaitForSingleObject(external_shutdown_event, 0U) == WAIT_OBJECT_0) {
      runtime.RequestStop();
      continue;
    }

    if (options.hold_timeout_ms.has_value() &&
        (::GetTickCount64() - started_at) >= *options.hold_timeout_ms) {
      runtime.RequestStop();
    }
  }

  if (external_shutdown_event != nullptr) {
    ::CloseHandle(external_shutdown_event);
  }

  return std::monostate{};
}

void PrintDiscoveryDiagnostics(ServiceRuntime& runtime) {
  ::Sleep(1000);

  const auto devices_result = runtime.ListDevices();
  if (!devices_result.ok()) {
    std::cerr << "Failed to list discovered devices: " << devices_result.error().message << "\n";
    return;
  }

  const auto mounts_result = runtime.ListMounts();
  if (!mounts_result.ok()) {
    std::cerr << "Failed to list active mounts: " << mounts_result.error().message << "\n";
    return;
  }

  std::wcout << L"Discovery diagnostics:\n";
  if (devices_result.value().empty()) {
    std::wcout << L"  devices: none\n";
  }

  for (const auto& device : devices_result.value()) {
    std::wcout << L"  device: " << device.device_path << L"\n";
    if (device.probe_error.has_value()) {
      std::cout << "    probe_error: " << orchard::blockio::ToString(device.probe_error->code)
                << " - " << device.probe_error->message << "\n";
      continue;
    }
    if (device.volumes.empty()) {
      std::wcout << L"    volumes: none\n";
      continue;
    }
    for (const auto& volume : device.volumes) {
      std::cout << "    volume oid=" << volume.object_id << " name=\"" << volume.name
                << "\" policy=" << orchard::apfs::ToString(volume.policy_action) << "\n";
      if (volume.mount.has_value()) {
        std::wcout << L"      mount: " << volume.mount->mount_point << L" (id="
                   << volume.mount->mount_id << L")\n";
      } else {
        std::wcout << L"      mount: none\n";
      }
      if (volume.mount_error.has_value()) {
        std::cout << "      mount_error: " << orchard::blockio::ToString(volume.mount_error->code)
                  << " - " << volume.mount_error->message << "\n";
      }
    }
  }

  std::wcout << L"  active mounts: " << mounts_result.value().size() << L"\n";
  for (const auto& mount : mounts_result.value()) {
    std::wcout << L"    " << mount.mount_point << L" <- ";
    std::wcout << mount.target_path.wstring() << L" (id=" << mount.mount_id << L")\n";
  }
}

void PrintPerformanceDiagnostics(ServiceRuntime& runtime) {
  const auto mounts_result = runtime.ListMounts();
  if (!mounts_result.ok()) {
    std::cerr << "Failed to list active mounts: " << mounts_result.error().message << "\n";
    return;
  }

  std::wcout << L"Performance diagnostics:\n";
  if (mounts_result.value().empty()) {
    std::wcout << L"  mounts: none\n";
    return;
  }

  for (const auto& mount : mounts_result.value()) {
    std::wcout << L"  mount " << mount.mount_id << L": " << mount.mount_point << L" <- "
               << mount.target_path.wstring() << L"\n";
    std::cout << "    apfs.path_lookup_calls=" << mount.performance.apfs.path_lookup_calls
              << " path_components_walked=" << mount.performance.apfs.path_components_walked
              << " directory_enumerations=" << mount.performance.apfs.path_directory_enumerations
              << "\n";
    std::cout << "    apfs.inode_cache hits=" << mount.performance.apfs.inode_cache_hits
              << " misses=" << mount.performance.apfs.inode_cache_misses << "\n";
    std::cout << "    apfs.block_cache hits=" << mount.performance.apfs.block_cache.cache_hits
              << " misses=" << mount.performance.apfs.block_cache.cache_misses
              << " device_reads=" << mount.performance.apfs.block_cache.device_reads
              << " requested_bytes=" << mount.performance.apfs.block_cache.requested_bytes
              << " fetched_bytes=" << mount.performance.apfs.block_cache.fetched_bytes << "\n";
    std::cout << "    apfs.block_cache.metadata hits="
              << mount.performance.apfs.block_cache.metadata.cache_hits
              << " misses=" << mount.performance.apfs.block_cache.metadata.cache_misses
              << " device_reads=" << mount.performance.apfs.block_cache.metadata.device_reads
              << " requested_bytes=" << mount.performance.apfs.block_cache.metadata.requested_bytes
              << " fetched_bytes=" << mount.performance.apfs.block_cache.metadata.fetched_bytes
              << "\n";
    std::cout << "    apfs.block_cache.file_data hits="
              << mount.performance.apfs.block_cache.file_data.cache_hits
              << " misses=" << mount.performance.apfs.block_cache.file_data.cache_misses
              << " device_reads=" << mount.performance.apfs.block_cache.file_data.device_reads
              << " requested_bytes=" << mount.performance.apfs.block_cache.file_data.requested_bytes
              << " fetched_bytes=" << mount.performance.apfs.block_cache.file_data.fetched_bytes
              << "\n";
    std::cout << "    mounted.resolve_file_node_calls="
              << mount.performance.mounted_volume.resolve_file_node_calls
              << " cache_hits=" << mount.performance.mounted_volume.resolve_file_node_cache_hits
              << " directory_hits=" << mount.performance.mounted_volume.directory_cache_hits
              << " directory_misses=" << mount.performance.mounted_volume.directory_cache_misses
              << "\n";
    std::cout << "    mounted.symlink_target_loads="
              << mount.performance.mounted_volume.symlink_target_loads
              << " read_extent_hits=" << mount.performance.mounted_volume.read_extent_cache_hits
              << " read_extent_misses=" << mount.performance.mounted_volume.read_extent_cache_misses
              << " read_ahead_hits=" << mount.performance.mounted_volume.read_ahead_hits
              << " read_ahead_prefetches=" << mount.performance.mounted_volume.read_ahead_prefetches
              << " compression_info_hits="
              << mount.performance.mounted_volume.compression_info_cache_hits
              << " compression_info_misses="
              << mount.performance.mounted_volume.compression_info_cache_misses
              << " compression_hits=" << mount.performance.mounted_volume.compression_cache_hits
              << " compression_misses=" << mount.performance.mounted_volume.compression_cache_misses
              << "\n";
    std::cout << "    mounted.read_requests requested_bytes="
              << mount.performance.mounted_volume.read_requested_bytes
              << " fetched_bytes=" << mount.performance.mounted_volume.read_fetched_bytes
              << " small=" << mount.performance.mounted_volume.read_requests_small
              << " medium=" << mount.performance.mounted_volume.read_requests_medium
              << " large=" << mount.performance.mounted_volume.read_requests_large
              << " sequential=" << mount.performance.mounted_volume.read_requests_sequential
              << " non_sequential=" << mount.performance.mounted_volume.read_requests_non_sequential
              << " large_file_small="
              << mount.performance.mounted_volume.large_file_small_request_reads
              << " large_file_small_sequential="
              << mount.performance.mounted_volume.large_file_small_request_sequential_reads << "\n";
    std::cout << "    mounted.open_read_counts zero="
              << mount.performance.mounted_volume.open_handles_zero_reads
              << " one=" << mount.performance.mounted_volume.open_handles_one_read
              << " two_to_four=" << mount.performance.mounted_volume.open_handles_two_to_four_reads
              << " five_plus=" << mount.performance.mounted_volume.open_handles_five_plus_reads
              << "\n";

    bool printed_callback = false;
    for (std::size_t index = 0; index < mount.performance.callbacks.callbacks.size(); ++index) {
      const auto& callback = mount.performance.callbacks.callbacks[index];
      if (callback.call_count == 0U) {
        continue;
      }

      if (!printed_callback) {
        std::cout << "    callbacks:\n";
        printed_callback = true;
      }
      const auto average_microseconds =
          callback.call_count == 0U ? 0U : callback.total_microseconds / callback.call_count;
      std::cout << "      "
                << orchard::fs_winfsp::ToString(
                       static_cast<orchard::fs_winfsp::MountCallbackId>(index))
                << ": count=" << callback.call_count << " total_us=" << callback.total_microseconds
                << " avg_us=" << average_microseconds << " max_us=" << callback.max_microseconds
                << " slow_50ms=" << callback.slow_over_50ms
                << " slow_200ms=" << callback.slow_over_200ms << "\n";
    }
    if (!printed_callback) {
      std::cout << "    callbacks: none\n";
    }
  }
}

int RunConsoleHost(const ServiceHostOptions& options) {
  ServiceRuntime runtime(options.service);
  auto start_result = runtime.Start();
  if (!start_result.ok()) {
    std::cerr << "Failed to start the Orchard service runtime: " << start_result.error().message
              << "\n";
    return 1;
  }

  auto mount_result = MaybeMountStartupVolume(runtime, options);
  if (!mount_result.ok()) {
    std::cerr << "Failed to mount the startup volume: " << mount_result.error().message << "\n";
    runtime.Stop();
    return 1;
  }

  std::wcout << L"Orchard service runtime is running in console mode.\n";
  if (options.diagnose_discovery) {
    PrintDiscoveryDiagnostics(runtime);
  }
  g_console_runtime = &runtime;
  ::SetConsoleCtrlHandler(&ConsoleControlHandler, TRUE);
  const auto wait_result = WaitForConsoleShutdown(runtime, options);
  ::SetConsoleCtrlHandler(&ConsoleControlHandler, FALSE);
  g_console_runtime = nullptr;

  if (options.diagnose_perf) {
    PrintPerformanceDiagnostics(runtime);
  }

  runtime.Stop();

  if (!wait_result.ok()) {
    std::cerr << wait_result.error().message << "\n";
    return 1;
  }

  return 0;
}

} // namespace

blockio::Result<ServiceHostOptions> ParseServiceHostCommandLine(const int argc, char** argv) {
  ServiceHostOptions options;
  std::optional<orchard::fs_winfsp::MountConfig> startup_mount;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--console") {
      options.mode = ServiceLaunchMode::kConsole;
      continue;
    }
    if (argument == "--install") {
      options.mode = ServiceLaunchMode::kInstall;
      continue;
    }
    if (argument == "--uninstall") {
      options.mode = ServiceLaunchMode::kUninstall;
      continue;
    }
    if (argument == "--service-name" && index + 1 < argc) {
      auto wide_result = ParseWideArgument(ParseWideArgumentRequest{
          .text = argv[++index],
          .name = "--service-name",
      });
      if (!wide_result.ok()) {
        return wide_result.error();
      }
      options.service.service_name = std::move(wide_result.value());
      continue;
    }
    if (argument == "--display-name" && index + 1 < argc) {
      auto wide_result = ParseWideArgument(ParseWideArgumentRequest{
          .text = argv[++index],
          .name = "--display-name",
      });
      if (!wide_result.ok()) {
        return wide_result.error();
      }
      options.service.display_name = std::move(wide_result.value());
      continue;
    }
    if ((argument == "--target" || argument == "-t") && index + 1 < argc) {
      if (!startup_mount.has_value()) {
        startup_mount.emplace();
      }
      startup_mount->target_path = argv[++index];
      continue;
    }
    if ((argument == "--mountpoint" || argument == "-m") && index + 1 < argc) {
      if (!startup_mount.has_value()) {
        startup_mount.emplace();
      }
      auto wide_result = ParseWideArgument(ParseWideArgumentRequest{
          .text = argv[++index],
          .name = "--mountpoint",
      });
      if (!wide_result.ok()) {
        return wide_result.error();
      }
      startup_mount->mount_point = std::move(wide_result.value());
      continue;
    }
    if (argument == "--volume-name" && index + 1 < argc) {
      if (!startup_mount.has_value()) {
        startup_mount.emplace();
      }
      startup_mount->selector.name = std::string(argv[++index]);
      continue;
    }
    if (argument == "--volume-oid" && index + 1 < argc) {
      if (!startup_mount.has_value()) {
        startup_mount.emplace();
      }
      auto value_result = ParseUint64(argv[++index]);
      if (!value_result.ok()) {
        return value_result.error();
      }
      startup_mount->selector.object_id = value_result.value();
      continue;
    }
    if (argument == "--hold-ms" && index + 1 < argc) {
      auto value_result = ParseUint64(argv[++index]);
      if (!value_result.ok() ||
          value_result.value() >
              static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                     "Invalid --hold-ms value.");
      }
      options.hold_timeout_ms = static_cast<std::uint32_t>(value_result.value());
      continue;
    }
    if (argument == "--shutdown-event" && index + 1 < argc) {
      auto wide_result = ParseWideArgument(ParseWideArgumentRequest{
          .text = argv[++index],
          .name = "--shutdown-event",
      });
      if (!wide_result.ok()) {
        return wide_result.error();
      }
      options.shutdown_event_name = std::move(wide_result.value());
      continue;
    }
    if (argument == "--diagnose-discovery") {
      options.diagnose_discovery = true;
      continue;
    }
    if (argument == "--diagnose-perf") {
      options.diagnose_perf = true;
      continue;
    }

    return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                 "Unknown orchard-service-host argument: " + std::string(argument));
  }

  if (startup_mount.has_value()) {
    if (startup_mount->target_path.empty() || startup_mount->mount_point.empty()) {
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "Startup mounts require both --target and --mountpoint.");
    }
    options.startup_mount = MountRequest{
        .mount_id = std::nullopt,
        .config = std::move(*startup_mount),
    };
  }

  return options;
}

int RunServiceHost(const ServiceHostOptions& options) {
  switch (options.mode) {
  case ServiceLaunchMode::kConsole:
    return RunConsoleHost(options);
  case ServiceLaunchMode::kInstall: {
    const auto install_result = InstallServiceBinary(options);
    if (!install_result.ok()) {
      std::cerr << install_result.error().message << "\n";
      return 1;
    }
    std::wcout << L"Installed Orchard service '" << options.service.service_name << L"'.\n";
    return 0;
  }
  case ServiceLaunchMode::kUninstall: {
    const auto uninstall_result = UninstallServiceBinary(options.service);
    if (!uninstall_result.ok()) {
      std::cerr << uninstall_result.error().message << "\n";
      return 1;
    }
    std::wcout << L"Uninstalled Orchard service '" << options.service.service_name << L"'.\n";
    return 0;
  }
  case ServiceLaunchMode::kServiceDispatcher: {
    WindowsServiceHost host(options);
    return host.RunDispatcher();
  }
  }

  return 1;
}

} // namespace orchard::mount_service
