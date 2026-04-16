#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "orchard/mount_service/device_discovery.h"
#include "orchard/mount_service/device_inventory.h"
#include "orchard/mount_service/mount_registry.h"
#include "orchard/mount_service/runtime.h"
#include "orchard/mount_service/service_host.h"
#include "orchard/mount_service/service_state.h"
#include "orchard_test/test.h"

namespace {

struct FakeFactoryState {
  std::vector<orchard::fs_winfsp::MountConfig> seen_configs;
  std::vector<std::shared_ptr<int>> stop_counters;
};

struct FakePosterState {
  std::vector<std::function<void()>> tasks;

  bool Post(std::function<void()> task) {
    tasks.push_back(std::move(task));
    return true;
  }

  void RunAll() {
    while (!tasks.empty()) {
      auto task = std::move(tasks.front());
      tasks.erase(tasks.begin());
      task();
    }
  }
};

class FakeDeviceMonitor final : public orchard::mount_service::DeviceMonitor {
public:
  [[nodiscard]] orchard::blockio::Result<std::monostate>
  Start(orchard::mount_service::DeviceMonitorCallback callback) override {
    callback_ = std::move(callback);
    started_ = true;
    return std::monostate{};
  }

  void Stop() noexcept override {
    started_ = false;
    callback_ = {};
    tracked_paths_.clear();
  }

  [[nodiscard]] orchard::blockio::Result<std::monostate>
  TrackMountedDevice(std::wstring_view device_path) override {
    tracked_paths_.emplace_back(device_path);
    return std::monostate{};
  }

  void UntrackMountedDevice(std::wstring_view device_path) noexcept override {
    tracked_paths_.erase(
        std::remove_if(tracked_paths_.begin(), tracked_paths_.end(),
                       [device_path](const std::wstring& current) {
                         return orchard::mount_service::NormalizeDevicePathKey(current) ==
                                orchard::mount_service::NormalizeDevicePathKey(device_path);
                       }),
        tracked_paths_.end());
  }

  void Emit(const orchard::mount_service::DeviceMonitorEvent& event) const {
    if (callback_) {
      callback_(event);
    }
  }

  [[nodiscard]] bool IsTracked(std::wstring_view device_path) const {
    return std::find_if(tracked_paths_.begin(), tracked_paths_.end(),
                        [device_path](const std::wstring& current) {
                          return orchard::mount_service::NormalizeDevicePathKey(current) ==
                                 orchard::mount_service::NormalizeDevicePathKey(device_path);
                        }) != tracked_paths_.end();
  }

private:
  orchard::mount_service::DeviceMonitorCallback callback_;
  std::vector<std::wstring> tracked_paths_;
  bool started_ = false;
};

class FakeDeviceEnumerator final : public orchard::mount_service::DeviceEnumerator {
public:
  explicit FakeDeviceEnumerator(std::vector<orchard::mount_service::DeviceInterfaceInfo> devices)
      : devices_(std::move(devices)) {}

  [[nodiscard]] orchard::blockio::Result<std::vector<orchard::mount_service::DeviceInterfaceInfo>>
  EnumeratePresentDiskInterfaces() override {
    return devices_;
  }

  void set_devices(std::vector<orchard::mount_service::DeviceInterfaceInfo> devices) {
    devices_ = std::move(devices);
  }

private:
  std::vector<orchard::mount_service::DeviceInterfaceInfo> devices_;
};

class FakeDeviceProber final : public orchard::mount_service::DeviceProber {
public:
  struct ProbeEntry {
    orchard::mount_service::KnownDeviceRecord record;
    std::optional<orchard::blockio::Error> error;
  };

  void SetResult(std::wstring device_path, ProbeEntry entry) {
    results_.push_back(std::pair(std::move(device_path), std::move(entry)));
  }

  [[nodiscard]] orchard::blockio::Result<orchard::mount_service::KnownDeviceRecord>
  Probe(const orchard::mount_service::DeviceInterfaceInfo& device) override {
    const auto found = std::find_if(results_.begin(), results_.end(), [&](const auto& current) {
      return orchard::mount_service::NormalizeDevicePathKey(current.first) ==
             orchard::mount_service::NormalizeDevicePathKey(device.device_path);
    });
    if (found == results_.end()) {
      return orchard::mount_service::KnownDeviceRecord{
          .device_path = device.device_path,
          .target_info = {},
          .volumes = {},
          .probe_error = std::nullopt,
      };
    }
    if (found->second.error.has_value()) {
      return *found->second.error;
    }
    return found->second.record;
  }

private:
  std::vector<std::pair<std::wstring, ProbeEntry>> results_;
};

class FakeMountPointAllocator final : public orchard::mount_service::MountPointAllocator {
public:
  void PushMountPoint(std::wstring mount_point) {
    mount_points_.push_back(std::move(mount_point));
  }

  [[nodiscard]] orchard::blockio::Result<std::wstring>
  Allocate(std::wstring_view device_path, const orchard::mount_service::KnownVolumeRecord& volume,
           const std::vector<std::wstring>& active_mount_points) override {
    (void)device_path;
    (void)volume;
    (void)active_mount_points;

    if (mount_points_.empty()) {
      return orchard::mount_service::MakeMountServiceError(
          orchard::blockio::ErrorCode::kOpenFailed,
          "The fake mount-point allocator has no more mount points.");
    }

    auto mount_point = std::move(mount_points_.front());
    mount_points_.erase(mount_points_.begin());
    return mount_point;
  }

private:
  std::vector<std::wstring> mount_points_;
};

struct FakeMountOps {
  std::vector<orchard::mount_service::MountRequest> mount_requests;
  std::vector<std::wstring> unmount_ids;
  std::vector<orchard::mount_service::MountedSessionRecord> active_mounts;
  int next_mount_ordinal = 1;
  std::optional<orchard::blockio::Error> mount_error_override;

  [[nodiscard]] orchard::blockio::Result<orchard::mount_service::MountedSessionRecord>
  Mount(const orchard::mount_service::MountRequest& request) {
    if (mount_error_override.has_value()) {
      return *mount_error_override;
    }
    mount_requests.push_back(request);
    auto record = orchard::mount_service::MountedSessionRecord{
        .mount_id = L"auto-" + std::to_wstring(next_mount_ordinal++),
        .target_path = request.config.target_path,
        .mount_point = request.config.mount_point,
        .volume_object_id = request.config.selector.object_id.value_or(0),
        .volume_name = request.config.selector.name.value_or("AutoMountedVolume"),
        .volume_label = L"AutoMountedVolume",
        .read_only = request.config.require_read_only_mount,
        .performance = {},
    };
    active_mounts.push_back(record);
    return record;
  }

  [[nodiscard]] orchard::blockio::Result<std::monostate>
  Unmount(const orchard::mount_service::UnmountRequest& request) {
    unmount_ids.push_back(request.mount_id);
    active_mounts.erase(
        std::remove_if(active_mounts.begin(), active_mounts.end(),
                       [&request](const orchard::mount_service::MountedSessionRecord& current) {
                         return current.mount_id == request.mount_id;
                       }),
        active_mounts.end());
    return std::monostate{};
  }

  [[nodiscard]] orchard::blockio::Result<std::vector<orchard::mount_service::MountedSessionRecord>>
  ListMounts() const {
    return active_mounts;
  }
};

class FakeManagedMountSession final : public orchard::mount_service::ManagedMountSession {
public:
  FakeManagedMountSession(std::wstring mount_point, std::wstring volume_label,
                          const std::uint64_t volume_object_id, std::string volume_name,
                          std::shared_ptr<int> stop_counter,
                          orchard::mount_service::MountedSessionPerformanceRecord performance)
      : mount_point_(std::move(mount_point)), volume_label_(std::move(volume_label)),
        volume_object_id_(volume_object_id), volume_name_(std::move(volume_name)),
        stop_counter_(std::move(stop_counter)), performance_(std::move(performance)) {}

  [[nodiscard]] std::wstring_view mount_point() const noexcept override {
    return mount_point_;
  }
  [[nodiscard]] std::wstring_view volume_label() const noexcept override {
    return volume_label_;
  }
  [[nodiscard]] std::uint64_t volume_object_id() const noexcept override {
    return volume_object_id_;
  }
  [[nodiscard]] std::string_view volume_name() const noexcept override {
    return volume_name_;
  }
  [[nodiscard]] orchard::mount_service::MountedSessionPerformanceRecord
  performance() const noexcept override {
    return performance_;
  }

  void Stop() noexcept override {
    ++(*stop_counter_);
  }

private:
  std::wstring mount_point_;
  std::wstring volume_label_;
  std::uint64_t volume_object_id_ = 0;
  std::string volume_name_;
  std::shared_ptr<int> stop_counter_;
  orchard::mount_service::MountedSessionPerformanceRecord performance_;
};

class FakeMountSessionFactory final : public orchard::mount_service::MountSessionFactory {
public:
  explicit FakeMountSessionFactory(std::shared_ptr<FakeFactoryState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] orchard::blockio::Result<orchard::mount_service::ManagedMountSessionHandle>
  Start(const orchard::fs_winfsp::MountConfig& config) override {
    state_->seen_configs.push_back(config);
    auto stop_counter = std::make_shared<int>(0);
    state_->stop_counters.push_back(stop_counter);
    orchard::mount_service::MountedSessionPerformanceRecord performance;
    performance.apfs.path_lookup_calls = 7U;
    performance.apfs.path_components_walked = 21U;
    performance.mounted_volume.directory_cache_hits = 11U;
    performance.callbacks
        .callbacks[static_cast<std::size_t>(orchard::fs_winfsp::MountCallbackId::kOpen)]
        .call_count = 3U;

    orchard::mount_service::ManagedMountSessionHandle session(
        new FakeManagedMountSession(config.mount_point, L"Fake Volume", 42U, "Fake Volume",
                                    std::move(stop_counter), std::move(performance)));
    return session;
  }

private:
  std::shared_ptr<FakeFactoryState> state_;
};

orchard::mount_service::MountRequest MakeMountRequest(std::wstring mount_id,
                                                      std::wstring mount_point) {
  orchard::mount_service::MountRequest request;
  request.mount_id = std::move(mount_id);
  request.config.target_path = "fixture.img";
  request.config.mount_point = std::move(mount_point);
  return request;
}

orchard::mount_service::KnownDeviceRecord MakeMountedCandidateDevice(std::wstring device_path,
                                                                     const std::uint64_t object_id,
                                                                     std::string volume_name) {
  orchard::mount_service::KnownDeviceRecord record;
  record.device_path = std::move(device_path);
  record.target_info.path = record.device_path;
  record.target_info.kind = orchard::blockio::TargetKind::kRawDevice;
  record.target_info.probe_candidate = true;
  record.volumes.push_back(orchard::mount_service::KnownVolumeRecord{
      .object_id = object_id,
      .name = std::move(volume_name),
      .policy_action = orchard::apfs::MountDisposition::kMountReadOnly,
      .policy_reasons = {},
      .policy_summary = "Readable and mountable for Orchard M3 tests.",
      .mount = std::nullopt,
      .mount_error = std::nullopt,
  });
  return record;
}

void ServiceStateMachineAcceptsExpectedTransitions() {
  orchard::mount_service::ServiceStateMachine state_machine;

  auto start_pending =
      state_machine.TransitionTo(orchard::mount_service::ServiceState::kStartPending, 2000U);
  ORCHARD_TEST_REQUIRE(start_pending.ok());
  ORCHARD_TEST_REQUIRE(start_pending.value().state ==
                       orchard::mount_service::ServiceState::kStartPending);
  ORCHARD_TEST_REQUIRE(start_pending.value().checkpoint == 1U);

  auto running = state_machine.TransitionTo(orchard::mount_service::ServiceState::kRunning);
  ORCHARD_TEST_REQUIRE(running.ok());
  ORCHARD_TEST_REQUIRE(running.value().accepts_stop);

  auto invalid = state_machine.TransitionTo(orchard::mount_service::ServiceState::kStartPending);
  ORCHARD_TEST_REQUIRE(!invalid.ok());

  auto stop_pending =
      state_machine.TransitionTo(orchard::mount_service::ServiceState::kStopPending, 1500U);
  ORCHARD_TEST_REQUIRE(stop_pending.ok());
  auto stopped = state_machine.TransitionTo(orchard::mount_service::ServiceState::kStopped);
  ORCHARD_TEST_REQUIRE(stopped.ok());
}

void MountRegistryRejectsDuplicateMountIdsAndMountPoints() {
  auto state = std::make_shared<FakeFactoryState>();
  orchard::mount_service::MountRegistry registry(std::make_unique<FakeMountSessionFactory>(state));

  auto first_mount = registry.MountVolume(MakeMountRequest(L"alpha", L"R:"));
  ORCHARD_TEST_REQUIRE(first_mount.ok());

  auto duplicate_id = registry.MountVolume(MakeMountRequest(L"alpha", L"S:"));
  ORCHARD_TEST_REQUIRE(!duplicate_id.ok());

  auto duplicate_mount_point = registry.MountVolume(MakeMountRequest(L"beta", L"r:"));
  ORCHARD_TEST_REQUIRE(!duplicate_mount_point.ok());

  auto mounts = registry.ListMounts();
  ORCHARD_TEST_REQUIRE(mounts.size() == 1U);
  ORCHARD_TEST_REQUIRE(mounts.front().performance.apfs.path_lookup_calls == 7U);
  ORCHARD_TEST_REQUIRE(mounts.front().performance.apfs.path_components_walked == 21U);
  ORCHARD_TEST_REQUIRE(mounts.front().performance.mounted_volume.directory_cache_hits == 11U);
  ORCHARD_TEST_REQUIRE(
      mounts.front()
          .performance.callbacks
          .callbacks[static_cast<std::size_t>(orchard::fs_winfsp::MountCallbackId::kOpen)]
          .call_count == 3U);

  auto unmount_result =
      registry.UnmountVolume(orchard::mount_service::UnmountRequest{.mount_id = L"alpha"});
  ORCHARD_TEST_REQUIRE(unmount_result.ok());
  ORCHARD_TEST_REQUIRE(state->stop_counters.size() == 1U);
  ORCHARD_TEST_REQUIRE(*state->stop_counters.front() == 1);
}

void ServiceRuntimeStopIsIdempotentAndStopsMountedSessions() {
  auto state = std::make_shared<FakeFactoryState>();
  std::vector<orchard::mount_service::ServiceState> seen_states;

  orchard::mount_service::ServiceRuntime runtime(
      {}, std::make_unique<FakeMountSessionFactory>(state),
      [&seen_states](const orchard::mount_service::ServiceStateSnapshot& snapshot) {
        seen_states.push_back(snapshot.state);
      },
      false);

  auto start_result = runtime.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());

  auto mount_result = runtime.MountVolume(MakeMountRequest(L"runtime-alpha", L"R:"));
  ORCHARD_TEST_REQUIRE(mount_result.ok());

  auto list_result = runtime.ListMounts();
  ORCHARD_TEST_REQUIRE(list_result.ok());
  ORCHARD_TEST_REQUIRE(list_result.value().size() == 1U);

  runtime.Stop();
  runtime.Stop();

  ORCHARD_TEST_REQUIRE(!seen_states.empty());
  ORCHARD_TEST_REQUIRE(seen_states.front() == orchard::mount_service::ServiceState::kStartPending);
  ORCHARD_TEST_REQUIRE(seen_states.back() == orchard::mount_service::ServiceState::kStopped);
  ORCHARD_TEST_REQUIRE(state->stop_counters.size() == 1U);
  ORCHARD_TEST_REQUIRE(*state->stop_counters.front() == 1);
}

void ServiceHostCommandLineParsesConsoleMountOptions() {
  std::vector<char*> argv{
      const_cast<char*>("orchard-service-host"),
      const_cast<char*>("--console"),
      const_cast<char*>("--service-name"),
      const_cast<char*>("OrchardTestSvc"),
      const_cast<char*>("--target"),
      const_cast<char*>("fixture.img"),
      const_cast<char*>("--mountpoint"),
      const_cast<char*>("R:"),
      const_cast<char*>("--volume-name"),
      const_cast<char*>("Data"),
      const_cast<char*>("--diagnose-discovery"),
      const_cast<char*>("--diagnose-perf"),
      const_cast<char*>("--hold-ms"),
      const_cast<char*>("5000"),
  };

  auto parse_result = orchard::mount_service::ParseServiceHostCommandLine(
      static_cast<int>(argv.size()), argv.data());
  ORCHARD_TEST_REQUIRE(parse_result.ok());
  ORCHARD_TEST_REQUIRE(parse_result.value().mode ==
                       orchard::mount_service::ServiceLaunchMode::kConsole);
  ORCHARD_TEST_REQUIRE(parse_result.value().service.service_name == L"OrchardTestSvc");
  ORCHARD_TEST_REQUIRE(parse_result.value().diagnose_discovery);
  ORCHARD_TEST_REQUIRE(parse_result.value().diagnose_perf);
  const auto& startup_mount_optional = parse_result.value().startup_mount;
  if (!startup_mount_optional.has_value()) {
    throw orchard_test::Failure("startup_mount was not populated.");
  }
  const auto& startup_mount = *startup_mount_optional;
  ORCHARD_TEST_REQUIRE(startup_mount.config.mount_point == L"R:");
  const auto& selector_name_optional = startup_mount.config.selector.name;
  if (!selector_name_optional.has_value()) {
    throw orchard_test::Failure("selector.name was not populated.");
  }
  const auto& selector_name = *selector_name_optional;
  ORCHARD_TEST_REQUIRE(selector_name == "Data");
  const auto& hold_timeout_optional = parse_result.value().hold_timeout_ms;
  if (!hold_timeout_optional.has_value()) {
    throw orchard_test::Failure("hold_timeout_ms was not populated.");
  }
  const auto hold_timeout_ms = *hold_timeout_optional;
  ORCHARD_TEST_REQUIRE(hold_timeout_ms == 5000U);
}

void ServiceHostCommandLineParsesInstallStartupMountOptions() {
  std::vector<char*> argv{
      const_cast<char*>("orchard-service-host"),
      const_cast<char*>("--install"),
      const_cast<char*>("--service-name"),
      const_cast<char*>("OrchardInstallSvc"),
      const_cast<char*>("--target"),
      const_cast<char*>(R"(C:\fixtures\plain-user-data.img)"),
      const_cast<char*>("--mountpoint"),
      const_cast<char*>("R:"),
      const_cast<char*>("--volume-oid"),
      const_cast<char*>("1026"),
  };

  auto parse_result = orchard::mount_service::ParseServiceHostCommandLine(
      static_cast<int>(argv.size()), argv.data());
  ORCHARD_TEST_REQUIRE(parse_result.ok());
  ORCHARD_TEST_REQUIRE(parse_result.value().mode ==
                       orchard::mount_service::ServiceLaunchMode::kInstall);
  ORCHARD_TEST_REQUIRE(parse_result.value().service.service_name == L"OrchardInstallSvc");
  ORCHARD_TEST_REQUIRE(parse_result.value().startup_mount.has_value());

  const auto& startup_mount = *parse_result.value().startup_mount;
  ORCHARD_TEST_REQUIRE(startup_mount.config.target_path == R"(C:\fixtures\plain-user-data.img)");
  ORCHARD_TEST_REQUIRE(startup_mount.config.mount_point == L"R:");
  ORCHARD_TEST_REQUIRE(startup_mount.config.selector.object_id.has_value());
  ORCHARD_TEST_REQUIRE(*startup_mount.config.selector.object_id == 1026U);
}

void DeviceInventoryDiffsAddedAndRemovedPaths() {
  orchard::mount_service::DeviceInventory inventory;
  inventory.UpsertDevice(MakeMountedCandidateDevice(LR"(\\.\PhysicalDrive7)", 7U, "Alpha"));

  const auto diff = inventory.DiffAgainst({
      orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive8)"},
      orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive7)"},
  });

  ORCHARD_TEST_REQUIRE(diff.added_paths.size() == 1U);
  ORCHARD_TEST_REQUIRE(diff.added_paths.front() == LR"(\\.\PhysicalDrive8)");
  ORCHARD_TEST_REQUIRE(diff.removed_paths.empty());
  ORCHARD_TEST_REQUIRE(diff.retained_paths.size() == 1U);
}

void RescanCoordinatorCoalescesBurstRequests() {
  auto poster = std::make_shared<FakePosterState>();
  int executions = 0;

  orchard::mount_service::RescanCoordinator coordinator(
      [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
      [&executions]() { ++executions; });

  coordinator.RequestRescan();
  coordinator.RequestRescan();
  ORCHARD_TEST_REQUIRE(poster->tasks.size() == 1U);

  poster->RunAll();
  ORCHARD_TEST_REQUIRE(executions == 2);
}

void DeviceDiscoveryManagerStartupEnumeratesAndMountsSupportedVolume() {
  auto poster = std::make_shared<FakePosterState>();
  auto monitor = std::make_unique<FakeDeviceMonitor>();
  auto* monitor_ptr = monitor.get();
  auto enumerator = std::make_unique<FakeDeviceEnumerator>(
      std::vector<orchard::mount_service::DeviceInterfaceInfo>{
          orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive9)"},
      });
  auto prober = std::make_unique<FakeDeviceProber>();
  prober->SetResult(LR"(\\.\PhysicalDrive9)",
                    FakeDeviceProber::ProbeEntry{
                        .record = MakeMountedCandidateDevice(LR"(\\.\PhysicalDrive9)", 9U, "Data"),
                        .error = std::nullopt,
                    });
  auto allocator = std::make_unique<FakeMountPointAllocator>();
  allocator->PushMountPoint(L"R:");
  FakeMountOps mount_ops;

  orchard::mount_service::DeviceDiscoveryManager manager(
      std::move(monitor), std::move(enumerator), std::move(prober), std::move(allocator),
      orchard::mount_service::DeviceDiscoveryCallbacks{
          .post_task =
              [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
          .mount_volume =
              [&mount_ops](const orchard::mount_service::MountRequest& request) {
                return mount_ops.Mount(request);
              },
          .unmount_volume =
              [&mount_ops](const orchard::mount_service::UnmountRequest& request) {
                return mount_ops.Unmount(request);
              },
          .list_mounts = [&mount_ops]() { return mount_ops.ListMounts(); },
      });

  auto start_result = manager.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());
  poster->RunAll();

  const auto devices = manager.ListDevices();
  ORCHARD_TEST_REQUIRE(devices.size() == 1U);
  ORCHARD_TEST_REQUIRE(devices.front().volumes.size() == 1U);
  const auto& mounted_volume_optional = devices.front().volumes.front().mount;
  if (!mounted_volume_optional.has_value()) {
    throw orchard_test::Failure("mounted volume binding was not populated.");
  }
  const auto& mounted_volume = *mounted_volume_optional;
  ORCHARD_TEST_REQUIRE(mounted_volume.mount_point == L"R:");
  ORCHARD_TEST_REQUIRE(mount_ops.mount_requests.size() == 1U);
  ORCHARD_TEST_REQUIRE(monitor_ptr->IsTracked(LR"(\\.\PhysicalDrive9)"));
}

void DeviceDiscoveryManagerAdoptsExistingMatchingMount() {
  auto poster = std::make_shared<FakePosterState>();
  auto monitor = std::make_unique<FakeDeviceMonitor>();
  auto enumerator = std::make_unique<FakeDeviceEnumerator>(
      std::vector<orchard::mount_service::DeviceInterfaceInfo>{
          orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive14)"},
      });
  auto prober = std::make_unique<FakeDeviceProber>();
  prober->SetResult(LR"(\\.\PhysicalDrive14)", FakeDeviceProber::ProbeEntry{
                                                   .record = MakeMountedCandidateDevice(
                                                       LR"(\\.\PhysicalDrive14)", 14U, "Data"),
                                                   .error = std::nullopt,
                                               });
  auto allocator = std::make_unique<FakeMountPointAllocator>();
  allocator->PushMountPoint(L"S:");
  FakeMountOps mount_ops;
  mount_ops.active_mounts.push_back(orchard::mount_service::MountedSessionRecord{
      .mount_id = L"existing-1",
      .target_path = LR"(\\.\PhysicalDrive14)",
      .mount_point = L"R:",
      .volume_object_id = 14U,
      .volume_name = "Data",
      .volume_label = L"Data",
      .read_only = true,
      .performance = {},
  });

  orchard::mount_service::DeviceDiscoveryManager manager(
      std::move(monitor), std::move(enumerator), std::move(prober), std::move(allocator),
      orchard::mount_service::DeviceDiscoveryCallbacks{
          .post_task =
              [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
          .mount_volume =
              [&mount_ops](const orchard::mount_service::MountRequest& request) {
                return mount_ops.Mount(request);
              },
          .unmount_volume =
              [&mount_ops](const orchard::mount_service::UnmountRequest& request) {
                return mount_ops.Unmount(request);
              },
          .list_mounts = [&mount_ops]() { return mount_ops.ListMounts(); },
      });

  auto start_result = manager.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());
  poster->RunAll();

  const auto devices = manager.ListDevices();
  ORCHARD_TEST_REQUIRE(devices.size() == 1U);
  ORCHARD_TEST_REQUIRE(devices.front().volumes.size() == 1U);
  ORCHARD_TEST_REQUIRE(devices.front().volumes.front().mount.has_value());
  ORCHARD_TEST_REQUIRE(devices.front().volumes.front().mount->mount_id == L"existing-1");
  ORCHARD_TEST_REQUIRE(devices.front().volumes.front().mount->mount_point == L"R:");
  ORCHARD_TEST_REQUIRE(mount_ops.mount_requests.empty());
}

void DeviceDiscoveryManagerRemovalUnmountsMountedDevice() {
  auto poster = std::make_shared<FakePosterState>();
  auto monitor = std::make_unique<FakeDeviceMonitor>();
  auto* monitor_ptr = monitor.get();
  auto enumerator = std::make_unique<FakeDeviceEnumerator>(
      std::vector<orchard::mount_service::DeviceInterfaceInfo>{
          orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive10)"},
      });
  auto* enumerator_ptr = enumerator.get();
  auto prober = std::make_unique<FakeDeviceProber>();
  prober->SetResult(LR"(\\.\PhysicalDrive10)", FakeDeviceProber::ProbeEntry{
                                                   .record = MakeMountedCandidateDevice(
                                                       LR"(\\.\PhysicalDrive10)", 10U, "Detach"),
                                                   .error = std::nullopt,
                                               });
  auto allocator = std::make_unique<FakeMountPointAllocator>();
  allocator->PushMountPoint(L"R:");
  FakeMountOps mount_ops;

  orchard::mount_service::DeviceDiscoveryManager manager(
      std::move(monitor), std::move(enumerator), std::move(prober), std::move(allocator),
      orchard::mount_service::DeviceDiscoveryCallbacks{
          .post_task =
              [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
          .mount_volume =
              [&mount_ops](const orchard::mount_service::MountRequest& request) {
                return mount_ops.Mount(request);
              },
          .unmount_volume =
              [&mount_ops](const orchard::mount_service::UnmountRequest& request) {
                return mount_ops.Unmount(request);
              },
          .list_mounts = [&mount_ops]() { return mount_ops.ListMounts(); },
      });

  auto start_result = manager.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());
  poster->RunAll();

  enumerator_ptr->set_devices({});
  monitor_ptr->Emit(orchard::mount_service::DeviceMonitorEvent{
      .kind = orchard::mount_service::DeviceMonitorEventKind::kInterfaceChange,
      .device_path = {},
  });
  poster->RunAll();

  ORCHARD_TEST_REQUIRE(mount_ops.unmount_ids.size() == 1U);
  ORCHARD_TEST_REQUIRE(manager.ListDevices().empty());
  ORCHARD_TEST_REQUIRE(!monitor_ptr->IsTracked(LR"(\\.\PhysicalDrive10)"));
}

void DeviceDiscoveryManagerBurstEventsDoNotDuplicateMounts() {
  auto poster = std::make_shared<FakePosterState>();
  auto monitor = std::make_unique<FakeDeviceMonitor>();
  auto* monitor_ptr = monitor.get();
  auto enumerator = std::make_unique<FakeDeviceEnumerator>(
      std::vector<orchard::mount_service::DeviceInterfaceInfo>{});
  auto* enumerator_ptr = enumerator.get();
  auto prober = std::make_unique<FakeDeviceProber>();
  prober->SetResult(LR"(\\.\PhysicalDrive11)", FakeDeviceProber::ProbeEntry{
                                                   .record = MakeMountedCandidateDevice(
                                                       LR"(\\.\PhysicalDrive11)", 11U, "Burst"),
                                                   .error = std::nullopt,
                                               });
  auto allocator = std::make_unique<FakeMountPointAllocator>();
  allocator->PushMountPoint(L"R:");
  FakeMountOps mount_ops;

  orchard::mount_service::DeviceDiscoveryManager manager(
      std::move(monitor), std::move(enumerator), std::move(prober), std::move(allocator),
      orchard::mount_service::DeviceDiscoveryCallbacks{
          .post_task =
              [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
          .mount_volume =
              [&mount_ops](const orchard::mount_service::MountRequest& request) {
                return mount_ops.Mount(request);
              },
          .unmount_volume =
              [&mount_ops](const orchard::mount_service::UnmountRequest& request) {
                return mount_ops.Unmount(request);
              },
          .list_mounts = [&mount_ops]() { return mount_ops.ListMounts(); },
      });

  auto start_result = manager.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());
  poster->RunAll();

  enumerator_ptr->set_devices({
      orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive11)"},
  });
  monitor_ptr->Emit(orchard::mount_service::DeviceMonitorEvent{
      .kind = orchard::mount_service::DeviceMonitorEventKind::kInterfaceChange,
      .device_path = {},
  });
  monitor_ptr->Emit(orchard::mount_service::DeviceMonitorEvent{
      .kind = orchard::mount_service::DeviceMonitorEventKind::kInterfaceChange,
      .device_path = {},
  });
  poster->RunAll();

  ORCHARD_TEST_REQUIRE(mount_ops.mount_requests.size() == 1U);
}

void DeviceDiscoveryManagerQueryRemoveSuppresssImmediateRemountUntilFailureClearsIt() {
  auto poster = std::make_shared<FakePosterState>();
  auto monitor = std::make_unique<FakeDeviceMonitor>();
  auto* monitor_ptr = monitor.get();
  auto enumerator = std::make_unique<FakeDeviceEnumerator>(
      std::vector<orchard::mount_service::DeviceInterfaceInfo>{
          orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive12)"},
      });
  auto prober = std::make_unique<FakeDeviceProber>();
  prober->SetResult(LR"(\\.\PhysicalDrive12)", FakeDeviceProber::ProbeEntry{
                                                   .record = MakeMountedCandidateDevice(
                                                       LR"(\\.\PhysicalDrive12)", 12U, "Hotplug"),
                                                   .error = std::nullopt,
                                               });
  auto allocator = std::make_unique<FakeMountPointAllocator>();
  allocator->PushMountPoint(L"R:");
  allocator->PushMountPoint(L"S:");
  FakeMountOps mount_ops;

  orchard::mount_service::DeviceDiscoveryManager manager(
      std::move(monitor), std::move(enumerator), std::move(prober), std::move(allocator),
      orchard::mount_service::DeviceDiscoveryCallbacks{
          .post_task =
              [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
          .mount_volume =
              [&mount_ops](const orchard::mount_service::MountRequest& request) {
                return mount_ops.Mount(request);
              },
          .unmount_volume =
              [&mount_ops](const orchard::mount_service::UnmountRequest& request) {
                return mount_ops.Unmount(request);
              },
          .list_mounts = [&mount_ops]() { return mount_ops.ListMounts(); },
      });

  auto start_result = manager.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());
  poster->RunAll();

  monitor_ptr->Emit(orchard::mount_service::DeviceMonitorEvent{
      .kind = orchard::mount_service::DeviceMonitorEventKind::kMountedDeviceQueryRemove,
      .device_path = LR"(\\.\PhysicalDrive12)",
  });
  poster->RunAll();

  auto devices = manager.ListDevices();
  ORCHARD_TEST_REQUIRE(devices.size() == 1U);
  ORCHARD_TEST_REQUIRE(!devices.front().volumes.front().mount.has_value());
  ORCHARD_TEST_REQUIRE(mount_ops.mount_requests.size() == 1U);
  ORCHARD_TEST_REQUIRE(mount_ops.unmount_ids.size() == 1U);

  monitor_ptr->Emit(orchard::mount_service::DeviceMonitorEvent{
      .kind = orchard::mount_service::DeviceMonitorEventKind::kMountedDeviceQueryRemoveFailed,
      .device_path = LR"(\\.\PhysicalDrive12)",
  });
  poster->RunAll();

  devices = manager.ListDevices();
  ORCHARD_TEST_REQUIRE(devices.front().volumes.front().mount.has_value());
  ORCHARD_TEST_REQUIRE(mount_ops.mount_requests.size() == 2U);
}

void DeviceDiscoveryManagerRecordsMountFailuresOnKnownVolume() {
  auto poster = std::make_shared<FakePosterState>();
  auto monitor = std::make_unique<FakeDeviceMonitor>();
  auto enumerator = std::make_unique<FakeDeviceEnumerator>(
      std::vector<orchard::mount_service::DeviceInterfaceInfo>{
          orchard::mount_service::DeviceInterfaceInfo{.device_path = LR"(\\.\PhysicalDrive13)"},
      });
  auto prober = std::make_unique<FakeDeviceProber>();
  prober->SetResult(LR"(\\.\PhysicalDrive13)", FakeDeviceProber::ProbeEntry{
                                                   .record = MakeMountedCandidateDevice(
                                                       LR"(\\.\PhysicalDrive13)", 13U, "Failure"),
                                                   .error = std::nullopt,
                                               });
  auto allocator = std::make_unique<FakeMountPointAllocator>();
  allocator->PushMountPoint(L"R:");
  FakeMountOps mount_ops;
  mount_ops.mount_error_override = orchard::mount_service::MakeMountServiceError(
      orchard::blockio::ErrorCode::kOpenFailed, "Synthetic mount failure.");

  orchard::mount_service::DeviceDiscoveryManager manager(
      std::move(monitor), std::move(enumerator), std::move(prober), std::move(allocator),
      orchard::mount_service::DeviceDiscoveryCallbacks{
          .post_task =
              [poster](std::function<void()> task) { return poster->Post(std::move(task)); },
          .mount_volume =
              [&mount_ops](const orchard::mount_service::MountRequest& request) {
                return mount_ops.Mount(request);
              },
          .unmount_volume =
              [&mount_ops](const orchard::mount_service::UnmountRequest& request) {
                return mount_ops.Unmount(request);
              },
          .list_mounts = [&mount_ops]() { return mount_ops.ListMounts(); },
      });

  auto start_result = manager.Start();
  ORCHARD_TEST_REQUIRE(start_result.ok());
  poster->RunAll();

  const auto devices = manager.ListDevices();
  ORCHARD_TEST_REQUIRE(devices.size() == 1U);
  ORCHARD_TEST_REQUIRE(devices.front().volumes.size() == 1U);
  ORCHARD_TEST_REQUIRE(!devices.front().volumes.front().mount.has_value());
  ORCHARD_TEST_REQUIRE(devices.front().volumes.front().mount_error.has_value());
  ORCHARD_TEST_REQUIRE(devices.front().volumes.front().mount_error->message ==
                       "Synthetic mount failure.");
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"ServiceStateMachineAcceptsExpectedTransitions",
       &ServiceStateMachineAcceptsExpectedTransitions},
      {"MountRegistryRejectsDuplicateMountIdsAndMountPoints",
       &MountRegistryRejectsDuplicateMountIdsAndMountPoints},
      {"ServiceRuntimeStopIsIdempotentAndStopsMountedSessions",
       &ServiceRuntimeStopIsIdempotentAndStopsMountedSessions},
      {"ServiceHostCommandLineParsesConsoleMountOptions",
       &ServiceHostCommandLineParsesConsoleMountOptions},
      {"ServiceHostCommandLineParsesInstallStartupMountOptions",
       &ServiceHostCommandLineParsesInstallStartupMountOptions},
      {"DeviceInventoryDiffsAddedAndRemovedPaths", &DeviceInventoryDiffsAddedAndRemovedPaths},
      {"RescanCoordinatorCoalescesBurstRequests", &RescanCoordinatorCoalescesBurstRequests},
      {"DeviceDiscoveryManagerStartupEnumeratesAndMountsSupportedVolume",
       &DeviceDiscoveryManagerStartupEnumeratesAndMountsSupportedVolume},
      {"DeviceDiscoveryManagerAdoptsExistingMatchingMount",
       &DeviceDiscoveryManagerAdoptsExistingMatchingMount},
      {"DeviceDiscoveryManagerRemovalUnmountsMountedDevice",
       &DeviceDiscoveryManagerRemovalUnmountsMountedDevice},
      {"DeviceDiscoveryManagerBurstEventsDoNotDuplicateMounts",
       &DeviceDiscoveryManagerBurstEventsDoNotDuplicateMounts},
      {"DeviceDiscoveryManagerQueryRemoveSuppresssImmediateRemountUntilFailureClearsIt",
       &DeviceDiscoveryManagerQueryRemoveSuppresssImmediateRemountUntilFailureClearsIt},
      {"DeviceDiscoveryManagerRecordsMountFailuresOnKnownVolume",
       &DeviceDiscoveryManagerRecordsMountFailuresOnKnownVolume},
  });
}
