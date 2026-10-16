// SPDX-License-Identifier: MIT
#include <array>
#include <cstdint>
#include <new>
#include <squatch/dsp/registry.hpp>

namespace squatch::dsp {
namespace {

template <class T>
Block* construct(void* at) {
  return ::new (at) T();  // placement: no allocation
}

template <class T>
void destroy(Block* b) {
  static_cast<T*>(b)->~T();
}

template <class T>
constexpr BlockType entry() {
  static_assert(alignof(T) <= kBlockAlign && sizeof(T) <= kBlockSize, "raise kBlockAlign or kBlockSize");
  return {&T::kInfo, T::params(), sizeof(T), alignof(T), construct<T>, destroy<T>};
}

template <class... Core, class... Extra>
constexpr std::array<BlockType, sizeof...(Core) + sizeof...(Extra)> table(BlockList<Core...>, BlockList<Extra...>) {
  return {entry<Core>()..., entry<Extra>()...};
}

const auto kTypes = table(CoreBlocks{}, ExtraBlocks{});

}  // namespace

Span<const BlockType> blockTypes() { return {kTypes.data(), kTypes.size()}; }

const BlockType* findBlockType(const char* typeId) {
  if (!typeId) return nullptr;
  for (const BlockType& t : kTypes)
    if (sameKey(t.info->typeId, typeId)) return &t;
  return nullptr;
}

Block* createBlock(const char* typeId, void* storage, std::size_t bytes) {
  const BlockType* t = findBlockType(typeId);
  const bool fits = t && storage && bytes >= t->size && reinterpret_cast<std::uintptr_t>(storage) % t->align == 0;
  return fits ? t->construct(storage) : nullptr;
}

bool destroyBlock(Block* b) {
  const BlockType* t = b ? findBlockType(b->info().typeId) : nullptr;
  if (t) t->destroy(b);
  return t != nullptr;
}

}  // namespace squatch::dsp
