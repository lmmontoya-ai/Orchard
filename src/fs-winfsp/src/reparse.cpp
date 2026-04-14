#include "orchard/fs_winfsp/reparse.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "orchard/apfs/format.h"
#include "orchard/fs_winfsp/path_bridge.h"

namespace orchard::fs_winfsp {
namespace {

constexpr std::uint32_t kIoReparseTagSymlink = 0xA000000CU;
constexpr std::uint32_t kSymlinkFlagRelative = 0x1U;

struct OrchardSymbolicLinkReparseBuffer {
  std::uint32_t reparse_tag = 0;
  std::uint16_t reparse_data_length = 0;
  std::uint16_t reserved = 0;
  std::uint16_t substitute_name_offset = 0;
  std::uint16_t substitute_name_length = 0;
  std::uint16_t print_name_offset = 0;
  std::uint16_t print_name_length = 0;
  std::uint32_t flags = 0;
  wchar_t path_buffer[1];
};

bool IsUnsupportedTargetChar(const wchar_t value) noexcept {
  if (value < 0x20) {
    return true;
  }

  switch (value) {
  case L'"':
  case L'<':
  case L'>':
  case L'|':
  case L'?':
  case L'*':
  case L':':
    return true;
  default:
    return false;
  }
}

blockio::Result<std::wstring> NormalizeApfsSymlinkTarget(const std::string_view target) {
  if (target.empty()) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "APFS symlink target is empty.");
  }

  auto wide_result = Utf8ToWide(target);
  if (!wide_result.ok()) {
    return wide_result.error();
  }

  auto wide_target = std::move(wide_result.value());
  std::replace(wide_target.begin(), wide_target.end(), L'/', L'\\');
  for (const auto value : wide_target) {
    if (IsUnsupportedTargetChar(value)) {
      return orchard::apfs::MakeApfsError(
          blockio::ErrorCode::kUnsupportedTarget,
          "APFS symlink target contains characters unsupported by the current Windows adapter.");
    }
  }

  return wide_target;
}

std::wstring NormalizeMountRoot(std::wstring mount_point) {
  std::replace(mount_point.begin(), mount_point.end(), L'/', L'\\');
  while (!mount_point.empty() &&
         (mount_point.back() == L'\\' || mount_point.back() == L'/') &&
         !(mount_point.size() == 3U && mount_point[1] == L':')) {
    mount_point.pop_back();
  }

  if (mount_point.size() == 2U && mount_point[1] == L':') {
    mount_point.push_back(L'\\');
  }

  return mount_point;
}

} // namespace

blockio::Result<WindowsSymlinkTarget>
TranslateSymlinkTarget(const SymlinkReparseRequest& request) {
  auto normalized_target_result = NormalizeApfsSymlinkTarget(request.target);
  if (!normalized_target_result.ok()) {
    return normalized_target_result.error();
  }

  const auto& normalized_target = normalized_target_result.value();
  WindowsSymlinkTarget translated;
  translated.relative = request.target.empty() ? false : request.target.front() != '/';
  if (translated.relative) {
    translated.substitute_name = normalized_target;
    translated.print_name = normalized_target;
    return translated;
  }

  auto mount_root = NormalizeMountRoot(request.mount_point);
  if (mount_root.empty()) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "WinFsp mount point is empty.");
  }

  translated.print_name = mount_root;
  if (translated.print_name.back() != L'\\') {
    translated.print_name.push_back(L'\\');
  }
  if (!normalized_target.empty() && normalized_target.front() == L'\\') {
    translated.print_name += normalized_target.substr(1);
  } else {
    translated.print_name += normalized_target;
  }
  translated.substitute_name = L"\\??\\" + translated.print_name;
  return translated;
}

blockio::Result<std::vector<std::uint8_t>>
BuildSymlinkReparseData(const SymlinkReparseRequest& request) {
  auto translated_result = TranslateSymlinkTarget(request);
  if (!translated_result.ok()) {
    return translated_result.error();
  }

  const auto& translated = translated_result.value();
  const auto substitute_bytes =
      translated.substitute_name.size() * sizeof(std::wstring::value_type);
  const auto print_bytes = translated.print_name.size() * sizeof(std::wstring::value_type);
  const auto path_buffer_bytes = substitute_bytes + print_bytes;
  const auto total_bytes = offsetof(OrchardSymbolicLinkReparseBuffer, path_buffer) + path_buffer_bytes;

  std::vector<std::uint8_t> bytes(total_bytes, 0U);
  auto* buffer = reinterpret_cast<OrchardSymbolicLinkReparseBuffer*>(bytes.data());
  buffer->reparse_tag = kIoReparseTagSymlink;
  buffer->reparse_data_length =
      static_cast<std::uint16_t>(total_bytes - offsetof(OrchardSymbolicLinkReparseBuffer, substitute_name_offset));
  buffer->reserved = 0U;
  buffer->substitute_name_offset = 0U;
  buffer->substitute_name_length = static_cast<std::uint16_t>(substitute_bytes);
  buffer->print_name_offset = static_cast<std::uint16_t>(substitute_bytes);
  buffer->print_name_length = static_cast<std::uint16_t>(print_bytes);
  buffer->flags = translated.relative ? kSymlinkFlagRelative : 0U;

  auto* path_buffer = buffer->path_buffer;
  if (substitute_bytes != 0U) {
    std::copy_n(translated.substitute_name.data(), translated.substitute_name.size(), path_buffer);
  }
  if (print_bytes != 0U) {
    std::copy_n(translated.print_name.data(), translated.print_name.size(),
                reinterpret_cast<wchar_t*>(reinterpret_cast<std::uint8_t*>(path_buffer) +
                                           substitute_bytes));
  }

  return bytes;
}

} // namespace orchard::fs_winfsp
