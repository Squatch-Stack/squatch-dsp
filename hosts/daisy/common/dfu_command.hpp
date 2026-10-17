// SPDX-License-Identifier: MIT
// The "dfu" line the Mac sends over USB serial to put the board into the STM32 ROM bootloader
// (hosts/daisy/tools/flash.py). A matcher fed the bytes as they arrive, in any chunking: it fires on
// "dfu" followed by CR or LF, and on nothing else (a banner, a stray "d", "dfux\n"). Pure C++, host-tested.
#pragma once
#include <cstddef>
#include <cstdint>

namespace squatch::board {

class DfuMatcher {
 public:
  /// Feed received bytes. True once, when a whole "dfu\n" (or "dfu\r") line has been seen.
  bool feed(const std::uint8_t* data, std::size_t n) {
    bool fired = false;
    for (std::size_t i = 0; i < n; ++i) fired = step(data[i]) || fired;
    return fired;
  }

 private:
  bool step(std::uint8_t c) {
    static constexpr char kWord[] = "dfu";
    if (pos_ == 3) {
      pos_ = 0;
      if (c == '\n' || c == '\r') return true;
      // Not a clean line ("dfux"): fall through so a 'd' right after still starts a new match.
    }
    if (c == static_cast<std::uint8_t>(kWord[pos_])) {
      ++pos_;
    } else {
      pos_ = c == 'd' ? 1 : 0;
    }
    return false;
  }
  int pos_ = 0;
};

}  // namespace squatch::board
