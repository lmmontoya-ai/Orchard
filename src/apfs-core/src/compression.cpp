#include "orchard/apfs/compression.h"

#include <array>

namespace orchard::apfs {
namespace {

constexpr std::array<std::uint8_t, 4> kDecmpfsMagic{
    static_cast<std::uint8_t>('c'),
    static_cast<std::uint8_t>('m'),
    static_cast<std::uint8_t>('p'),
    static_cast<std::uint8_t>('f'),
};
constexpr std::uint32_t kDecmpfsAlgorithmUncompressedAttribute = 9U;

} // namespace

blockio::Result<CompressionInfo> ParseCompressionInfo(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return CompressionInfo{};
  }

  if (!HasRange(bytes, 0U, 16U)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Compression xattr is too small for a decmpfs header.");
  }

  if (!std::equal(kDecmpfsMagic.begin(), kDecmpfsMagic.end(), bytes.begin())) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Compression xattr is missing the expected decmpfs magic.");
  }

  const auto algorithm_id = ReadLe32(bytes, 0x04U);
  CompressionInfo info;
  info.algorithm_id = algorithm_id;
  info.uncompressed_size = ReadLe64(bytes, 0x08U);
  info.supported = algorithm_id == kDecmpfsAlgorithmUncompressedAttribute;
  info.kind = info.supported ? CompressionKind::kDecmpfsUncompressedAttribute
                             : CompressionKind::kUnsupported;
  return info;
}

blockio::Result<std::vector<std::uint8_t>>
DecodeCompressionPayload(const std::span<const std::uint8_t> bytes) {
  auto info_result = ParseCompressionInfo(bytes);
  if (!info_result.ok()) {
    return info_result.error();
  }

  const auto& info = info_result.value();
  if (!info.supported) {
    return MakeApfsError(blockio::ErrorCode::kNotImplemented,
                         "Compression algorithm is not yet supported by Orchard.");
  }

  const auto payload = bytes.subspan(16U);
  if (payload.size() < info.uncompressed_size) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Compression xattr payload is smaller than the advertised size.");
  }

  return std::vector<std::uint8_t>(
      payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(info.uncompressed_size));
}

std::string_view ToString(const CompressionKind kind) noexcept {
  switch (kind) {
  case CompressionKind::kNone:
    return "none";
  case CompressionKind::kDecmpfsUncompressedAttribute:
    return "decmpfs_uncompressed_attribute";
  case CompressionKind::kUnsupported:
    return "unsupported";
  }

  return "unsupported";
}

} // namespace orchard::apfs
