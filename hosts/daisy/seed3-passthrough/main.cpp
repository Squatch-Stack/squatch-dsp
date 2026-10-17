// SPDX-License-Identifier: MIT
// seed3-passthrough: first sound on a Seed3. AUDIO IN 1 -> AUDIO OUT 1 and AUDIO IN 2 -> AUDIO
// OUT 2, unchanged. The LED blinks at 1 Hz (5 Hz once a callback has been late or overrun), and
// once a second USB serial reports the callback's cost and timing (hosts/daisy/README.md says
// what each field means). The block size is SQUATCH_BLOCK (32 by default). Variant "crash" faults on purpose after 3 s,
// to prove that a crashed image lands in the ROM bootloader.
#include "../common/seed3.hpp"

#ifndef SQUATCH_CRASH
#define SQUATCH_CRASH 0
#endif

namespace {

using namespace squatch::board;

::daisy::DaisySeed hw;
UsbSerial serial;
Meter<2> meter;

void callback(::daisy::AudioHandle::InputBuffer in, ::daisy::AudioHandle::OutputBuffer out, size_t size) {
  const std::uint32_t start = Dwt::now();
  for (size_t i = 0; i < size; ++i) {
    out[0][i] = in[0][i];
    out[1][i] = in[1][i];
  }
  const float inPeak = peakOf(in[0], size);
  meter.endBlock(start, Dwt::now() - start, inPeak, inPeak);
}

}  // namespace

int main() {
  const BoardInfo board = boot(hw, serial);
  meter.configure(board.budget, board.blocksPerSecond);
  hw.StartAudio(callback);
  Window<2> w;
  std::uint32_t ungated = 0;
  bool alarm = false;
  for (;;) {
    ungated += ungateUsbPhyClock() ? 1 : 0;
    serial.serviceHost();
    heartbeat(hw, alarm);
#if SQUATCH_CRASH
    // Recovery test (variant "crash"): three seconds in, write to an address with no memory behind it. The bus fault
    // must end in the ROM bootloader, not a freeze (see resetToRomBootloader).
    if (::daisy::System::GetNow() > 3000) *reinterpret_cast<volatile std::uint32_t*>(0xA0000000u) = 1u;
#endif
    if (!meter.take(w)) continue;
    alarm = alarm || w.late > 0 || w.overruns > 0;
    if (w.seq % 10 == 1) banner(serial, board);
    reportCommon(serial, w, ungated);
  }
}
