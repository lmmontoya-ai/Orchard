#include "orchard/fs_winfsp/path_bridge.h"

#include <Windows.h>

#include <algorithm>
#include <vector>

#include "orchard/apfs/format.h"

namespace orchard::fs_winfsp {
namespace {

constexpr wchar_t kBackslash = L'\\';
constexpr wchar_t kSlash = L'/';

} // namespace

blockio::Result<std::string> WideToUtf8(const std::wstring_view text) {
  if (text.empty()) {
    return std::string{};
  }

  const auto size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr,
                                          nullptr);
  if (size <= 0) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Failed to convert UTF-16 text to UTF-8.");
  }

  std::string utf8(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), utf8.data(), size, nullptr,
                            nullptr) != size) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Failed to convert UTF-16 text to UTF-8.");
  }

  return utf8;
}

blockio::Result<std::wstring> Utf8ToWide(const std::string_view text) {
  if (text.empty()) {
    return std::wstring{};
  }

  const auto size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Failed to convert UTF-8 text to UTF-16.");
  }

  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), wide.data(), size) != size) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Failed to convert UTF-8 text to UTF-16.");
  }

  return wide;
}

blockio::Result<std::string> NormalizeWindowsPath(const std::wstring_view windows_path) {
  if (windows_path.empty() || windows_path == L"\\" || windows_path == L"/") {
    return std::string("/");
  }

  auto utf8_result = WideToUtf8(windows_path);
  if (!utf8_result.ok()) {
    return utf8_result.error();
  }

  std::string normalized;
  normalized.reserve(utf8_result.value().size() + 1U);
  normalized.push_back('/');

  bool previous_was_separator = true;
  for (const auto ch : utf8_result.value()) {
    if (ch == ':') {
      return orchard::apfs::MakeApfsError(
          blockio::ErrorCode::kNotImplemented,
          "WinFsp stream syntax is not supported by the Orchard M2 read-only adapter.");
    }

    const auto is_separator = ch == '\\' || ch == '/';
    if (is_separator) {
      if (!previous_was_separator) {
        normalized.push_back('/');
      }
      previous_was_separator = true;
      continue;
    }

    normalized.push_back(ch);
    previous_was_separator = false;
  }

  if (normalized.size() > 1U && normalized.back() == '/') {
    normalized.pop_back();
  }

  if (normalized.empty()) {
    normalized = "/";
  }

  return normalized;
}

blockio::Result<std::wstring> OrchardPathToWindowsPath(const std::string_view orchard_path) {
  auto wide_result = Utf8ToWide(orchard_path);
  if (!wide_result.ok()) {
    return wide_result.error();
  }

  auto windows_path = std::move(wide_result.value());
  if (windows_path.empty() || windows_path == L"/") {
    return std::wstring(L"\\");
  }

  std::replace(windows_path.begin(), windows_path.end(), kSlash, kBackslash);
  if (!windows_path.empty() && windows_path.front() != kBackslash) {
    windows_path.insert(windows_path.begin(), kBackslash);
  }

  return windows_path;
}

int CompareDirectoryNames(const std::wstring_view left, const std::wstring_view right,
                          const bool case_insensitive) noexcept {
  const auto result =
      ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                             static_cast<int>(right.size()), case_insensitive ? TRUE : FALSE);
  switch (result) {
  case CSTR_LESS_THAN:
    return -1;
  case CSTR_EQUAL:
    return 0;
  case CSTR_GREATER_THAN:
    return 1;
  default:
    return 0;
  }
}

} // namespace orchard::fs_winfsp
