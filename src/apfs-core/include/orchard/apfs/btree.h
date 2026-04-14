#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "orchard/apfs/object.h"

namespace orchard::apfs {

struct NodeLocation {
  std::uint16_t offset = 0;
  std::uint16_t length = 0;
};

struct BtreeInfoFixed {
  std::uint32_t flags = 0;
  std::uint32_t node_size = 0;
  std::uint32_t key_size = 0;
  std::uint32_t value_size = 0;
};

struct BtreeInfo {
  BtreeInfoFixed fixed;
  std::uint32_t longest_key = 0;
  std::uint32_t longest_value = 0;
  std::uint64_t key_count = 0;
  std::uint64_t node_count = 0;
};

struct NodeHeader {
  ObjectHeader object;
  std::uint16_t flags = 0;
  std::uint16_t level = 0;
  std::uint32_t record_count = 0;
  NodeLocation table_space;
  NodeLocation free_space;
  NodeLocation key_free_list;
  NodeLocation value_free_list;
};

struct NodeRecordView {
  std::span<const std::uint8_t> key;
  std::span<const std::uint8_t> value;
  std::size_t index = 0;
};

struct NodeRecordCopy {
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> value;
  std::size_t index = 0;
  std::uint64_t node_block_index = 0;
  std::uint16_t node_level = 0;
};

class NodeView {
public:
  using CompareFn = std::function<blockio::Result<int>(std::span<const std::uint8_t>)>;

  NodeView(std::span<const std::uint8_t> bytes, NodeHeader header, std::size_t key_area_start,
           std::size_t value_area_start, std::size_t value_area_end, std::optional<BtreeInfo> info,
           std::uint32_t fixed_key_size, std::uint32_t fixed_value_size);

  [[nodiscard]] const NodeHeader& header() const noexcept {
    return header_;
  }
  [[nodiscard]] bool is_root() const noexcept;
  [[nodiscard]] bool is_leaf() const noexcept;
  [[nodiscard]] bool uses_fixed_kv() const noexcept;
  [[nodiscard]] std::uint32_t fixed_key_size() const noexcept {
    return fixed_key_size_;
  }
  [[nodiscard]] std::uint32_t fixed_value_size() const noexcept {
    return fixed_value_size_;
  }
  [[nodiscard]] const std::optional<BtreeInfo>& btree_info() const noexcept {
    return info_;
  }

  [[nodiscard]] blockio::Result<NodeRecordView> RecordAt(std::size_t index) const;
  [[nodiscard]] blockio::Result<std::optional<std::size_t>>
  FindFloorIndex(const CompareFn& compare) const;
  [[nodiscard]] blockio::Result<std::optional<std::size_t>>
  FindLowerBoundIndex(const CompareFn& compare) const;

private:
  std::span<const std::uint8_t> bytes_;
  NodeHeader header_;
  std::size_t key_area_start_ = 0;
  std::size_t value_area_end_ = 0;
  std::optional<BtreeInfo> info_;
  std::uint32_t fixed_key_size_ = 0;
  std::uint32_t fixed_value_size_ = 0;
};

blockio::Result<NodeView> ParseNode(std::span<const std::uint8_t> block, std::uint32_t block_size);
blockio::Result<std::uint64_t> ParseChildBlockIndex(std::span<const std::uint8_t> value);

class BtreeWalker {
public:
  using CompareFn = NodeView::CompareFn;
  using VisitFn = std::function<blockio::Result<bool>(const NodeRecordView&)>;
  using ChildResolverFn = std::function<blockio::Result<std::uint64_t>(std::uint64_t)>;

  class Cursor {
  public:
    Cursor();
    ~Cursor();
    Cursor(Cursor&&) noexcept;
    Cursor& operator=(Cursor&&) noexcept;
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] blockio::Result<NodeRecordView> Current() const;
    [[nodiscard]] blockio::Result<NodeRecordCopy> CurrentCopy() const;
    [[nodiscard]] blockio::Result<bool> Advance();

  private:
    struct State;

    explicit Cursor(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;

    friend class BtreeWalker;
  };

  explicit BtreeWalker(const PhysicalObjectReader& reader,
                       ChildResolverFn child_resolver = ChildResolverFn{});

  [[nodiscard]] blockio::Result<std::optional<NodeRecordCopy>> Find(std::uint64_t root_block_index,
                                                                    const CompareFn& compare) const;
  [[nodiscard]] blockio::Result<std::optional<Cursor>> LowerBound(std::uint64_t root_block_index,
                                                                  const CompareFn& compare) const;
  [[nodiscard]] blockio::Result<std::size_t> VisitRange(std::uint64_t root_block_index,
                                                        const CompareFn& compare,
                                                        const VisitFn& visitor) const;
  [[nodiscard]] blockio::Result<std::size_t> VisitInOrder(std::uint64_t root_block_index,
                                                          const VisitFn& visitor) const;

private:
  [[nodiscard]] blockio::Result<ObjectBlock> ReadObject(std::uint64_t block_index) const;
  [[nodiscard]] blockio::Result<std::uint64_t>
  ResolveChildBlockIndex(std::uint64_t child_identifier) const;
  [[nodiscard]] blockio::Result<std::optional<Cursor>>
  BuildLowerBoundCursor(std::uint64_t root_block_index, const CompareFn& compare) const;
  [[nodiscard]] blockio::Result<bool> DescendToLeftmostLeaf(Cursor::State& state) const;
  [[nodiscard]] blockio::Result<bool> AdvanceCursor(Cursor::State& state) const;
  [[nodiscard]] blockio::Result<std::size_t>
  VisitNodeInOrder(std::uint64_t block_index, const VisitFn& visitor, std::size_t visited) const;

  const PhysicalObjectReader* reader_ = nullptr;
  ChildResolverFn child_resolver_;
};

} // namespace orchard::apfs
