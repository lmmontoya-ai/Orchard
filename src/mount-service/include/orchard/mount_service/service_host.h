#pragma once

#include <cstdint>
#include <optional>

#include "orchard/blockio/result.h"
#include "orchard/mount_service/types.h"

namespace orchard::mount_service {

enum class ServiceLaunchMode {
  kServiceDispatcher,
  kConsole,
  kInstall,
  kUninstall,
};

struct ServiceHostOptions {
  ServiceLaunchMode mode = ServiceLaunchMode::kServiceDispatcher;
  ServiceConfig service;
  std::optional<MountRequest> startup_mount;
  std::optional<std::uint32_t> hold_timeout_ms;
  std::optional<std::wstring> shutdown_event_name;
  bool diagnose_discovery = false;
  bool diagnose_perf = false;
};

[[nodiscard]] blockio::Result<ServiceHostOptions> ParseServiceHostCommandLine(int argc,
                                                                              char** argv);
int RunServiceHost(const ServiceHostOptions& options);

} // namespace orchard::mount_service
