// SPDX-License-Identifier: MIT
// The heap-free factory (Block interface v2): every core block by its type id, constructed in
// storage the caller owns. Nothing here allocates, so a pedal can build a board from a preset
// into a static buffer, and a plugin or LV2 host can look a block up by the id in a saved state.
//
//   alignas(kBlockAlign) static unsigned char at[kBlockSize];
//   Block* b = createBlock("cc.squatch.gate", at, sizeof at);  // nullptr: unknown, or no room
//   ...
//   destroyBlock(b);  // runs the destructor; the storage stays the caller's
//
// kBlockSize and kBlockAlign are enough for every registered block (the tuner is the largest,
// ~128 kB of fixed buffers). Blocks still take their working memory from an Arena in prepare().
//
// Blocks that exist only in the development tree register through registry_extra.hpp, which is
// never exported (it defines ExtraBlocks); without it the list is empty.
#pragma once
#include <cstddef>

#include "block.hpp"
#include "blocks/cabinet.hpp"
#include "blocks/claw.hpp"
#include "blocks/comp.hpp"
#include "blocks/crunch.hpp"
#include "blocks/eq.hpp"
#include "blocks/gate.hpp"
#include "blocks/hall.hpp"
#include "blocks/limiter.hpp"
#include "blocks/trail_od.hpp"
#include "blocks/tuner.hpp"
#include "span.hpp"

namespace squatch::dsp {

using CoreBlocks = BlockList<Cabinet, Claw, Compressor, Crunch, Eq, Gate, Hall, Limiter, TrailOD, TunerBlock>;

}  // namespace squatch::dsp

#if __has_include("registry_extra.hpp")
#include "registry_extra.hpp"
#else
namespace squatch::dsp {
using ExtraBlocks = BlockList<>;
}  // namespace squatch::dsp
#endif

namespace squatch::dsp {

struct BlockType {
  const BlockInfo* info;
  Span<const ParamDesc> params;
  std::size_t size;
  std::size_t align;
  Block* (*construct)(void* at);
  void (*destroy)(Block* b);
};

/// Every registered block type, in a fixed order.
Span<const BlockType> blockTypes();

/// nullptr when no block has this type id.
const BlockType* findBlockType(const char* typeId);

/// Constructs the block named by typeId at `storage`. nullptr when the id is unknown, or the
/// storage is too small or misaligned for it.
Block* createBlock(const char* typeId, void* storage, std::size_t bytes);

/// Destroys a block made by createBlock() (by its info().typeId). False if it was not one.
bool destroyBlock(Block* b);

namespace registry_detail {
template <class... T>
constexpr std::size_t largest(BlockList<T...>) {
  std::size_t n = 0;
  for (std::size_t s : {std::size_t{0}, sizeof(T)...}) n = s > n ? s : n;
  return n;
}
template <class... T>
constexpr std::size_t count(BlockList<T...>) {
  return sizeof...(T);
}
}  // namespace registry_detail

/// How many block types are registered: the core's five and any the development tree adds.
constexpr std::size_t kBlockTypes = registry_detail::count(CoreBlocks{}) + registry_detail::count(ExtraBlocks{});
constexpr std::size_t kBlockSize = registry_detail::largest(CoreBlocks{}) > registry_detail::largest(ExtraBlocks{})
                                       ? registry_detail::largest(CoreBlocks{})
                                       : registry_detail::largest(ExtraBlocks{});
constexpr std::size_t kBlockAlign = 64;

}  // namespace squatch::dsp
