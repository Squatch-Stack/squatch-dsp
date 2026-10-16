// SPDX-License-Identifier: MIT
// Writes a mono 32-bit float WAV, for impulse responses the tests make up.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace host {

inline bool writeWav(const std::string& path, const std::vector<float>& x, std::uint32_t rate) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::uint32_t data = static_cast<std::uint32_t>(x.size() * 4);
  const auto u32 = [f](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
  const auto u16 = [f](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16), u16(3), u16(1), u32(rate), u32(rate * 4), u16(4), u16(32);
  std::fwrite("data", 1, 4, f), u32(data), std::fwrite(x.data(), 4, x.size(), f);
  return std::fclose(f) == 0;
}

}  // namespace host
