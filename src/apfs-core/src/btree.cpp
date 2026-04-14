#include "orchard/apfs/btree.h"

namespace orchard::apfs {
namespace {

constexpr std::size_t kKvlocSize = 8U;
constexpr std::size_t kKvoffSize = 4U;

blockio::Result<NodeLocation> ReadNodeLocation(const std::span<const std::uint8_t> block,
                                               const std::size_t offset) {
  if (!HasRange(block, offset, 4U)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "B-tree node block is too small for a location descriptor.");
  }

  return NodeLocation{
      .offset = ReadLe16(block, offset),
      .length = ReadLe16(block, offset + 2U),
  };
}

blockio::Result<std::optional<BtreeInfo>>
TryReadBtreeInfo(const std::span<const std::uint8_t> block, const NodeHeader& header,
                 const std::uint32_t block_size) {
  if ((header.flags & kBtreeNodeFlagRoot) == 0U) {
    return std::optional<BtreeInfo>{};
  }

  if (block_size < kBtreeInfoSize ||
      !HasRange(block, block_size - kBtreeInfoSize, kBtreeInfoSize)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "B-tree root node does not have enough space for the footer info block.");
  }

  const auto footer_offset = static_cast<std::size_t>(block_size - kBtreeInfoSize);
  BtreeInfo info;
  info.fixed.flags = ReadLe32(block, footer_offset + 0x00U);
  info.fixed.node_size = ReadLe32(block, footer_offset + 0x04U);
  info.fixed.key_size = ReadLe32(block, footer_offset + 0x08U);
  info.fixed.value_size = ReadLe32(block, footer_offset + 0x0CU);
  info.longest_key = ReadLe32(block, footer_offset + 0x10U);
  info.longest_value = ReadLe32(block, footer_offset + 0x14U);
  info.key_count = ReadLe64(block, footer_offset + 0x18U);
  info.node_count = ReadLe64(block, footer_offset + 0x20U);

  if (info.fixed.node_size != 0U && info.fixed.node_size != block_size) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree root footer node size does not match the object block size.");
  }

  return std::optional<BtreeInfo>(info);
}

blockio::Result<std::pair<std::uint32_t, std::uint32_t>>
InferFixedRecordSizes(const NodeHeader& header, const std::optional<BtreeInfo>& info) {
  if ((header.flags & kBtreeNodeFlagFixedKv) == 0U) {
    return std::pair<std::uint32_t, std::uint32_t>{0U, 0U};
  }

  if (info.has_value()) {
    return std::pair<std::uint32_t, std::uint32_t>{info->fixed.key_size, info->fixed.value_size};
  }

  switch (header.object.subtype) {
  case kObjectTypeOmap:
    return std::pair<std::uint32_t, std::uint32_t>{
        16U, (header.flags & kBtreeNodeFlagLeaf) != 0U ? 16U : 8U};
  default:
    return MakeApfsError(blockio::ErrorCode::kNotImplemented,
                         "Fixed-size APFS B-tree nodes currently require a supported tree subtype "
                         "or a root footer.");
  }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
blockio::Result<NodeRecordView>
ParseVariableRecord(const std::span<const std::uint8_t> bytes, const NodeHeader& header,
                    const std::size_t key_area_start, const std::size_t value_area_end,
                    const std::size_t entry_offset, const std::size_t index) {
  if (!HasRange(bytes, entry_offset, kKvlocSize)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree node record table entry extends beyond the node header area.");
  }

  const auto key_offset = ReadLe16(bytes, entry_offset + 0x00U);
  const auto key_length = ReadLe16(bytes, entry_offset + 0x02U);
  const auto value_offset = ReadLe16(bytes, entry_offset + 0x04U);
  const auto value_length = ReadLe16(bytes, entry_offset + 0x06U);

  const auto key_absolute_offset = key_area_start + key_offset;
  if (!HasRange(bytes, key_absolute_offset, key_length)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree node key extends beyond the object block.");
  }

  if ((header.flags & kBtreeNodeFlagCheckKoffInvalid) != 0U &&
      value_offset == kBtreeOffsetInvalid) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree node value offset is marked invalid for a live record.");
  }

  if (value_offset > value_area_end) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree node value offset points before the start of the value area.");
  }

  const auto value_absolute_offset = value_area_end - value_offset;
  if (!HasRange(bytes, value_absolute_offset, value_length)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree node value extends beyond the object block.");
  }

  return NodeRecordView{
      .key =
          std::span(bytes.begin() + static_cast<std::ptrdiff_t>(key_absolute_offset), key_length),
      .value = std::span(bytes.begin() + static_cast<std::ptrdiff_t>(value_absolute_offset),
                         value_length),
      .index = index,
  };
}

blockio::Result<NodeRecordView>
ParseFixedRecord(const std::span<const std::uint8_t> bytes, const std::size_t key_area_start,
                 const std::size_t value_area_end, const std::uint32_t fixed_key_size,
                 const std::uint32_t fixed_value_size, const std::size_t entry_offset,
                 const std::size_t index) {
  if (!HasRange(bytes, entry_offset, kKvoffSize)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree fixed-size node record table entry extends beyond the node.");
  }

  const auto key_offset = ReadLe16(bytes, entry_offset + 0x00U);
  const auto value_offset = ReadLe16(bytes, entry_offset + 0x02U);
  const auto key_absolute_offset = key_area_start + key_offset;
  if (!HasRange(bytes, key_absolute_offset, fixed_key_size)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree fixed-size node key extends beyond the object block.");
  }

  if (value_offset == kBtreeOffsetInvalid) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree fixed-size node value offset is invalid for a live record.");
  }

  if (value_offset > value_area_end) {
    return MakeApfsError(
        blockio::ErrorCode::kCorruptData,
        "B-tree fixed-size node value offset points before the start of the value area.");
  }

  const auto value_absolute_offset = value_area_end - value_offset;
  if (!HasRange(bytes, value_absolute_offset, fixed_value_size)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree fixed-size node value extends beyond the object block.");
  }

  return NodeRecordView{
      .key = std::span(bytes.begin() + static_cast<std::ptrdiff_t>(key_absolute_offset),
                       fixed_key_size),
      .value = std::span(bytes.begin() + static_cast<std::ptrdiff_t>(value_absolute_offset),
                         fixed_value_size),
      .index = index,
  };
}
// NOLINTEND(bugprone-easily-swappable-parameters)

struct NodeRecordIdentity {
  std::uint64_t block_index = 0U;
  std::uint16_t level = 0U;
};

NodeRecordCopy CopyRecord(const NodeRecordView& record, const NodeRecordIdentity& identity) {
  NodeRecordCopy copy;
  copy.index = record.index;
  copy.node_block_index = identity.block_index;
  copy.node_level = identity.level;
  copy.key.assign(record.key.begin(), record.key.end());
  copy.value.assign(record.value.begin(), record.value.end());
  return copy;
}

} // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
NodeView::NodeView(std::span<const std::uint8_t> bytes, NodeHeader header,
                   const std::size_t key_area_start, const std::size_t /*value_area_start*/,
                   const std::size_t value_area_end, std::optional<BtreeInfo> info,
                   const std::uint32_t fixed_key_size, const std::uint32_t fixed_value_size)
    : bytes_(bytes), header_(header), key_area_start_(key_area_start),
      value_area_end_(value_area_end), info_(info), fixed_key_size_(fixed_key_size),
      fixed_value_size_(fixed_value_size) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool NodeView::is_root() const noexcept {
  return (header_.flags & kBtreeNodeFlagRoot) != 0U;
}

bool NodeView::is_leaf() const noexcept {
  return (header_.flags & kBtreeNodeFlagLeaf) != 0U;
}

bool NodeView::uses_fixed_kv() const noexcept {
  return (header_.flags & kBtreeNodeFlagFixedKv) != 0U;
}

blockio::Result<NodeRecordView> NodeView::RecordAt(const std::size_t index) const {
  if (index >= header_.record_count) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Requested B-tree record index is outside the node record count.");
  }

  const auto entry_size = uses_fixed_kv() ? kKvoffSize : kKvlocSize;
  const auto entry_offset = kBtreeNodeHeaderSize + (index * entry_size);
  if (uses_fixed_kv()) {
    return ParseFixedRecord(bytes_, key_area_start_, value_area_end_, fixed_key_size_,
                            fixed_value_size_, entry_offset, index);
  }

  return ParseVariableRecord(bytes_, header_, key_area_start_, value_area_end_, entry_offset,
                             index);
}

blockio::Result<std::optional<std::size_t>>
NodeView::FindFloorIndex(const CompareFn& compare) const {
  if (header_.record_count == 0U) {
    return std::optional<std::size_t>{};
  }

  std::int64_t left = 0;
  std::int64_t right = static_cast<std::int64_t>(header_.record_count) - 1;
  std::optional<std::size_t> floor_index;

  while (left <= right) {
    const auto current = left + ((right - left) / 2);
    auto record_result = RecordAt(static_cast<std::size_t>(current));
    if (!record_result.ok()) {
      return record_result.error();
    }

    auto compare_value_result = compare(record_result.value().key);
    if (!compare_value_result.ok()) {
      return compare_value_result.error();
    }

    if (compare_value_result.value() <= 0) {
      floor_index = static_cast<std::size_t>(current);
      left = current + 1;
    } else {
      right = current - 1;
    }
  }

  return floor_index;
}

blockio::Result<std::optional<std::size_t>>
NodeView::FindLowerBoundIndex(const CompareFn& compare) const {
  if (header_.record_count == 0U) {
    return std::optional<std::size_t>{};
  }

  std::size_t left = 0U;
  std::size_t right = header_.record_count;
  while (left < right) {
    const auto current = left + ((right - left) / 2U);
    auto record_result = RecordAt(current);
    if (!record_result.ok()) {
      return record_result.error();
    }

    auto compare_value_result = compare(record_result.value().key);
    if (!compare_value_result.ok()) {
      return compare_value_result.error();
    }

    if (compare_value_result.value() < 0) {
      left = current + 1U;
    } else {
      right = current;
    }
  }

  if (left >= header_.record_count) {
    return std::optional<std::size_t>{};
  }

  return std::optional<std::size_t>(left);
}

blockio::Result<NodeView> ParseNode(const std::span<const std::uint8_t> block,
                                    const std::uint32_t block_size) {
  if (block_size == 0U || block.size() < block_size || !HasRange(block, 0U, kBtreeNodeHeaderSize)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "APFS B-tree node block is too small for the node header.");
  }

  auto object_header_result = ParseObjectHeader(block);
  if (!object_header_result.ok()) {
    return object_header_result.error();
  }

  const auto& object_header = object_header_result.value();
  if (object_header.type != kObjectTypeBtreeNode && object_header.type != kObjectTypeBtree) {
    return MakeApfsError(blockio::ErrorCode::kInvalidFormat,
                         "APFS object is not a B-tree node or root object.");
  }

  NodeHeader header;
  header.object = object_header;
  header.flags = ReadLe16(block, 0x20U);
  header.level = ReadLe16(block, 0x22U);
  header.record_count = ReadLe32(block, 0x24U);

  if ((header.flags & ~kBtreeNodeFlagMask) != 0U) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS B-tree node advertises unsupported flag bits.");
  }

  auto table_space_result = ReadNodeLocation(block, 0x28U);
  if (!table_space_result.ok()) {
    return table_space_result.error();
  }
  header.table_space = table_space_result.value();

  auto free_space_result = ReadNodeLocation(block, 0x2CU);
  if (!free_space_result.ok()) {
    return free_space_result.error();
  }
  header.free_space = free_space_result.value();

  auto key_free_result = ReadNodeLocation(block, 0x30U);
  if (!key_free_result.ok()) {
    return key_free_result.error();
  }
  header.key_free_list = key_free_result.value();

  auto value_free_result = ReadNodeLocation(block, 0x34U);
  if (!value_free_result.ok()) {
    return value_free_result.error();
  }
  header.value_free_list = value_free_result.value();

  const auto entry_size = (header.flags & kBtreeNodeFlagFixedKv) != 0U ? kKvoffSize : kKvlocSize;
  const auto minimum_table_bytes =
      static_cast<std::uint64_t>(header.record_count) * static_cast<std::uint64_t>(entry_size);
  if (minimum_table_bytes > header.table_space.length) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS B-tree node record table is smaller than the advertised key count.");
  }

  const auto key_area_start =
      kBtreeNodeHeaderSize + header.table_space.offset + header.table_space.length;
  const auto value_area_start =
      key_area_start + header.free_space.offset + header.free_space.length;

  auto info_result = TryReadBtreeInfo(block, header, block_size);
  if (!info_result.ok()) {
    return info_result.error();
  }

  const auto footer_size = info_result.value().has_value() ? kBtreeInfoSize : 0U;
  const auto value_area_end = static_cast<std::size_t>(block_size - footer_size);
  if (key_area_start > value_area_end || value_area_start > value_area_end) {
    return MakeApfsError(
        blockio::ErrorCode::kCorruptData,
        "APFS B-tree node layout sections overlap or extend beyond the object block.");
  }

  auto fixed_size_result = InferFixedRecordSizes(header, info_result.value());
  if (!fixed_size_result.ok()) {
    return fixed_size_result.error();
  }

  return NodeView(block.first(block_size), header, key_area_start, value_area_start, value_area_end,
                  info_result.value(), fixed_size_result.value().first,
                  fixed_size_result.value().second);
}

blockio::Result<std::uint64_t> ParseChildBlockIndex(const std::span<const std::uint8_t> value) {
  if (value.size() < sizeof(std::uint64_t)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS internal B-tree record does not contain at least an 8-byte child "
                         "identifier.");
  }

  return ReadLe64(value, 0U);
}

struct BtreeWalker::Cursor::State {
  struct Frame {
    ObjectBlock object;
    std::size_t index = 0U;
  };

  const BtreeWalker* walker = nullptr;
  std::vector<Frame> frames;
};

BtreeWalker::Cursor::Cursor() = default;
BtreeWalker::Cursor::~Cursor() = default;
BtreeWalker::Cursor::Cursor(Cursor&&) noexcept = default;
BtreeWalker::Cursor& BtreeWalker::Cursor::operator=(Cursor&&) noexcept = default;

BtreeWalker::Cursor::Cursor(std::unique_ptr<State> state) : state_(std::move(state)) {}

bool BtreeWalker::Cursor::valid() const noexcept {
  return state_ != nullptr && !state_->frames.empty();
}

blockio::Result<NodeRecordView> BtreeWalker::Cursor::Current() const {
  if (!valid()) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "B-tree cursor does not currently point at a record.");
  }

  const auto& leaf_frame = state_->frames.back();
  auto node_result = ParseNode(leaf_frame.object.view(), state_->walker->reader_->block_size());
  if (!node_result.ok()) {
    return node_result.error();
  }
  if (!node_result.value().is_leaf()) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree cursor stopped on a non-leaf node.");
  }
  if (leaf_frame.index >= node_result.value().header().record_count) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "B-tree cursor record index is outside the leaf record count.");
  }

  return node_result.value().RecordAt(leaf_frame.index);
}

blockio::Result<NodeRecordCopy> BtreeWalker::Cursor::CurrentCopy() const {
  auto record_result = Current();
  if (!record_result.ok()) {
    return record_result.error();
  }

  const auto& leaf_frame = state_->frames.back();
  auto node_result = ParseNode(leaf_frame.object.view(), state_->walker->reader_->block_size());
  if (!node_result.ok()) {
    return node_result.error();
  }

  return CopyRecord(record_result.value(), NodeRecordIdentity{
                                               .block_index = leaf_frame.object.block_index,
                                               .level = node_result.value().header().level,
                                           });
}

blockio::Result<bool> BtreeWalker::Cursor::Advance() {
  if (!valid()) {
    return false;
  }

  auto advance_result = state_->walker->AdvanceCursor(*state_);
  if (!advance_result.ok()) {
    return advance_result.error();
  }
  if (!advance_result.value()) {
    state_->frames.clear();
  }
  return advance_result.value();
}

BtreeWalker::BtreeWalker(const PhysicalObjectReader& reader, ChildResolverFn child_resolver)
    : reader_(&reader), child_resolver_(std::move(child_resolver)) {}

blockio::Result<ObjectBlock> BtreeWalker::ReadObject(const std::uint64_t block_index) const {
  if (reader_ == nullptr) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "B-tree walker is not configured with a physical object reader.");
  }

  return reader_->ReadPhysicalObject(block_index);
}

blockio::Result<std::uint64_t>
BtreeWalker::ResolveChildBlockIndex(const std::uint64_t child_identifier) const {
  if (!child_resolver_) {
    return child_identifier;
  }
  return child_resolver_(child_identifier);
}

blockio::Result<std::optional<BtreeWalker::Cursor>>
BtreeWalker::BuildLowerBoundCursor(const std::uint64_t root_block_index,
                                   const CompareFn& compare) const {
  auto state = std::make_unique<Cursor::State>();
  state->walker = this;

  std::uint64_t current_block_index = root_block_index;
  for (std::size_t depth = 0; depth < 12U; ++depth) {
    auto object_result = ReadObject(current_block_index);
    if (!object_result.ok()) {
      return object_result.error();
    }

    auto node_result = ParseNode(object_result.value().view(), reader_->block_size());
    if (!node_result.ok()) {
      return node_result.error();
    }
    if (node_result.value().header().record_count == 0U) {
      return std::optional<Cursor>{};
    }

    Cursor::State::Frame frame{
        .object = std::move(object_result.value()),
        .index = 0U,
    };

    if (node_result.value().is_leaf()) {
      auto lower_bound_result = node_result.value().FindLowerBoundIndex(compare);
      if (!lower_bound_result.ok()) {
        return lower_bound_result.error();
      }

      const auto lower_bound_index = lower_bound_result.value();
      if (lower_bound_index.has_value()) {
        frame.index = *lower_bound_index;
        state->frames.push_back(std::move(frame));
        return std::optional<Cursor>(Cursor(std::move(state)));
      }

      frame.index = node_result.value().header().record_count;
      state->frames.push_back(std::move(frame));
      auto advance_result = AdvanceCursor(*state);
      if (!advance_result.ok()) {
        return advance_result.error();
      }
      if (!advance_result.value()) {
        return std::optional<Cursor>{};
      }
      return std::optional<Cursor>(Cursor(std::move(state)));
    }

    auto floor_index_result = node_result.value().FindFloorIndex(compare);
    if (!floor_index_result.ok()) {
      return floor_index_result.error();
    }

    frame.index = floor_index_result.value().value_or(0U);
    auto record_result = node_result.value().RecordAt(frame.index);
    if (!record_result.ok()) {
      return record_result.error();
    }

    auto child_identifier_result = ParseChildBlockIndex(record_result.value().value);
    if (!child_identifier_result.ok()) {
      return child_identifier_result.error();
    }
    auto child_block_result = ResolveChildBlockIndex(child_identifier_result.value());
    if (!child_block_result.ok()) {
      return child_block_result.error();
    }

    state->frames.push_back(std::move(frame));
    current_block_index = child_block_result.value();
  }

  return MakeApfsError(blockio::ErrorCode::kCorruptData,
                       "APFS B-tree height exceeded the supported traversal depth.");
}

blockio::Result<bool> BtreeWalker::DescendToLeftmostLeaf(Cursor::State& state) const {
  while (!state.frames.empty()) {
    auto& frame = state.frames.back();
    auto node_result = ParseNode(frame.object.view(), reader_->block_size());
    if (!node_result.ok()) {
      return node_result.error();
    }

    if (node_result.value().is_leaf()) {
      if (node_result.value().header().record_count == 0U) {
        return false;
      }
      frame.index = 0U;
      return true;
    }
    if (node_result.value().header().record_count == 0U) {
      return false;
    }

    frame.index = 0U;
    auto record_result = node_result.value().RecordAt(0U);
    if (!record_result.ok()) {
      return record_result.error();
    }

    auto child_identifier_result = ParseChildBlockIndex(record_result.value().value);
    if (!child_identifier_result.ok()) {
      return child_identifier_result.error();
    }
    auto child_block_result = ResolveChildBlockIndex(child_identifier_result.value());
    if (!child_block_result.ok()) {
      return child_block_result.error();
    }

    auto child_object_result = ReadObject(child_block_result.value());
    if (!child_object_result.ok()) {
      return child_object_result.error();
    }
    state.frames.push_back(Cursor::State::Frame{
        .object = std::move(child_object_result.value()),
        .index = 0U,
    });
  }

  return false;
}

blockio::Result<bool> BtreeWalker::AdvanceCursor(Cursor::State& state) const {
  if (state.frames.empty()) {
    return false;
  }

  auto& leaf_frame = state.frames.back();
  auto leaf_node_result = ParseNode(leaf_frame.object.view(), reader_->block_size());
  if (!leaf_node_result.ok()) {
    return leaf_node_result.error();
  }
  if (!leaf_node_result.value().is_leaf()) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "B-tree cursor is not currently positioned on a leaf node.");
  }

  if (leaf_frame.index + 1U < leaf_node_result.value().header().record_count) {
    ++leaf_frame.index;
    return true;
  }

  for (std::size_t depth = state.frames.size(); depth-- > 1U;) {
    auto& parent_frame = state.frames[depth - 1U];
    auto parent_node_result = ParseNode(parent_frame.object.view(), reader_->block_size());
    if (!parent_node_result.ok()) {
      return parent_node_result.error();
    }

    if (parent_frame.index + 1U >= parent_node_result.value().header().record_count) {
      continue;
    }

    ++parent_frame.index;
    state.frames.resize(depth);

    auto parent_record_result = parent_node_result.value().RecordAt(parent_frame.index);
    if (!parent_record_result.ok()) {
      return parent_record_result.error();
    }

    auto child_identifier_result = ParseChildBlockIndex(parent_record_result.value().value);
    if (!child_identifier_result.ok()) {
      return child_identifier_result.error();
    }
    auto child_block_result = ResolveChildBlockIndex(child_identifier_result.value());
    if (!child_block_result.ok()) {
      return child_block_result.error();
    }

    auto child_object_result = ReadObject(child_block_result.value());
    if (!child_object_result.ok()) {
      return child_object_result.error();
    }
    state.frames.push_back(Cursor::State::Frame{
        .object = std::move(child_object_result.value()),
        .index = 0U,
    });

    auto descend_result = DescendToLeftmostLeaf(state);
    if (!descend_result.ok()) {
      return descend_result.error();
    }
    if (descend_result.value()) {
      return true;
    }
  }

  state.frames.clear();
  return false;
}

blockio::Result<std::optional<NodeRecordCopy>>
BtreeWalker::Find(const std::uint64_t root_block_index, const CompareFn& compare) const {
  if (reader_ == nullptr) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "B-tree walker is not configured with a physical object reader.");
  }

  std::uint64_t current_block_index = root_block_index;
  for (std::size_t depth = 0; depth < 12U; ++depth) {
    auto object_result = reader_->ReadPhysicalObject(current_block_index);
    if (!object_result.ok()) {
      return object_result.error();
    }

    auto node_result = ParseNode(object_result.value().view(), reader_->block_size());
    if (!node_result.ok()) {
      return node_result.error();
    }

    auto floor_index_result = node_result.value().FindFloorIndex(compare);
    if (!floor_index_result.ok()) {
      return floor_index_result.error();
    }
    const auto floor_index = floor_index_result.value();
    if (!floor_index.has_value()) {
      return std::optional<NodeRecordCopy>{};
    }

    auto record_result = node_result.value().RecordAt(*floor_index);
    if (!record_result.ok()) {
      return record_result.error();
    }

    if (node_result.value().is_leaf()) {
      return std::optional<NodeRecordCopy>(
          CopyRecord(record_result.value(), NodeRecordIdentity{
                                                .block_index = current_block_index,
                                                .level = node_result.value().header().level,
                                            }));
    }

    auto child_result = ParseChildBlockIndex(record_result.value().value);
    if (!child_result.ok()) {
      return child_result.error();
    }
    auto child_block_result = ResolveChildBlockIndex(child_result.value());
    if (!child_block_result.ok()) {
      return child_block_result.error();
    }
    current_block_index = child_block_result.value();
  }

  return MakeApfsError(blockio::ErrorCode::kCorruptData,
                       "APFS B-tree height exceeded the supported traversal depth.");
}

blockio::Result<std::optional<BtreeWalker::Cursor>>
BtreeWalker::LowerBound(const std::uint64_t root_block_index, const CompareFn& compare) const {
  return BuildLowerBoundCursor(root_block_index, compare);
}

blockio::Result<std::size_t> BtreeWalker::VisitRange(const std::uint64_t root_block_index,
                                                     const CompareFn& compare,
                                                     const VisitFn& visitor) const {
  auto cursor_result = BuildLowerBoundCursor(root_block_index, compare);
  if (!cursor_result.ok()) {
    return cursor_result.error();
  }
  if (!cursor_result.value().has_value()) {
    return 0U;
  }

  auto& cursor_state = cursor_result.value();
  auto cursor = std::move(cursor_state).value_or(Cursor{});
  std::size_t total_visited = 0U;
  while (cursor.valid()) {
    auto record_result = cursor.Current();
    if (!record_result.ok()) {
      return record_result.error();
    }

    auto visitor_result = visitor(record_result.value());
    if (!visitor_result.ok()) {
      return visitor_result.error();
    }

    ++total_visited;
    if (!visitor_result.value()) {
      return total_visited;
    }

    auto advance_result = cursor.Advance();
    if (!advance_result.ok()) {
      return advance_result.error();
    }
    if (!advance_result.value()) {
      break;
    }
  }

  return total_visited;
}

blockio::Result<std::size_t> BtreeWalker::VisitInOrder(const std::uint64_t root_block_index,
                                                       const VisitFn& visitor) const {
  return VisitNodeInOrder(root_block_index, visitor, 0U);
}

blockio::Result<std::size_t> BtreeWalker::VisitNodeInOrder(const std::uint64_t block_index,
                                                           const VisitFn& visitor,
                                                           const std::size_t visited) const {
  auto object_result = reader_->ReadPhysicalObject(block_index);
  if (!object_result.ok()) {
    return object_result.error();
  }

  auto node_result = ParseNode(object_result.value().view(), reader_->block_size());
  if (!node_result.ok()) {
    return node_result.error();
  }

  std::size_t total_visited = visited;
  if (node_result.value().is_leaf()) {
    for (std::size_t index = 0; index < node_result.value().header().record_count; ++index) {
      auto record_result = node_result.value().RecordAt(index);
      if (!record_result.ok()) {
        return record_result.error();
      }

      auto visitor_result = visitor(record_result.value());
      if (!visitor_result.ok()) {
        return visitor_result.error();
      }

      ++total_visited;
      if (!visitor_result.value()) {
        return total_visited;
      }
    }

    return total_visited;
  }

  for (std::size_t index = 0; index < node_result.value().header().record_count; ++index) {
    auto record_result = node_result.value().RecordAt(index);
    if (!record_result.ok()) {
      return record_result.error();
    }

    auto child_result = ParseChildBlockIndex(record_result.value().value);
    if (!child_result.ok()) {
      return child_result.error();
    }

    auto child_block_result = ResolveChildBlockIndex(child_result.value());
    if (!child_block_result.ok()) {
      return child_block_result.error();
    }

    auto child_visit_result = VisitNodeInOrder(child_block_result.value(), visitor, total_visited);
    if (!child_visit_result.ok()) {
      return child_visit_result.error();
    }
    total_visited = child_visit_result.value();
  }

  return total_visited;
}

} // namespace orchard::apfs
