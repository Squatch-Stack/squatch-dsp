// SPDX-License-Identifier: MIT
// What every Seed3 firmware here shares on the board: the DWT cycle counter, USB serial lines,
// the libDaisy #716 workaround, the heartbeat LED, audio setup and the once-a-second report.
// Target-only (libDaisy); the measurement logic it drives is host-tested (meter.hpp).
//
// Build-time choices (the Makefile sets them per variant):
//   SQUATCH_BLOCK      samples per callback: 32 (default), 48 or 64
//   SQUATCH_FIRMWARE   the name printed in the banner
//   SQUATCH_LIBDAISY   the libDaisy commit the image was built against
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "daisy_seed.h"
#include "dfu_command.hpp"
#include "meter.hpp"

#ifndef SQUATCH_BLOCK
#define SQUATCH_BLOCK 32
#endif
#ifndef SQUATCH_FIRMWARE
#define SQUATCH_FIRMWARE "seed3"
#endif
#ifndef SQUATCH_LIBDAISY
#define SQUATCH_LIBDAISY "unknown"
#endif

namespace squatch::board {

/// Last resort for a crashed image: drive the Seed's BOOT0 helper pin (PG3) high, wait for its capacitor, and reset, so the
/// chip comes back in the STM32 ROM bootloader and the Mac can flash it without a button press. Written with direct
/// register access and a counted wait: a fault handler cannot rely on SysTick (System::ResetToBootloader waits on it).
[[noreturn]] inline void resetToRomBootloader() {
  RCC->AHB4ENR |= RCC_AHB4ENR_GPIOGEN;
  (void)RCC->AHB4ENR;
  GPIOG->MODER = (GPIOG->MODER & ~(3u << 6)) | (1u << 6);  // PG3 as an output
  GPIOG->BSRR = 1u << 3;                                    // high
  for (volatile std::uint32_t i = 0; i < 6000000u; ++i) {   // several ms at 480 MHz: the capacitor charges
  }
  NVIC_SystemReset();
  for (;;) {
  }
}

constexpr int kBlock = SQUATCH_BLOCK;
constexpr int kSampleRate = 48000;
static_assert(kBlock >= 1 && kBlock <= 256, "libDaisy's DMA buffer holds up to 256 samples per half");

/// The Cortex-M7 cycle counter: one tick per CPU clock, wraps every ~8.9 s at 480 MHz.
struct Dwt {
  static void enable() {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;  // the M7's DWT is locked out of reset
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  }
  static std::uint32_t now() { return DWT->CYCCNT; }
};

/// libDaisy #716: its USB suspend callback gates the PHY clock and nothing ungates it, so USB
/// serial goes silent after the first bus suspend (hosts send them around attach). Clearing
/// PCGCCTL.STOPCLK whenever it is found set is the reporter's workaround. Returns true when it
/// had to: a count above zero in the report is #716 reproducing on this board.
inline bool ungateUsbPhyClock() {
  auto* pcgcctl = reinterpret_cast<volatile std::uint32_t*>(USB2_OTG_FS_PERIPH_BASE + USB_OTG_PCGCCTL_BASE);
  if ((*pcgcctl & USB_OTG_PCGCCTL_STOPCLK) == 0) return false;
  *pcgcctl &= ~USB_OTG_PCGCCTL_STOPCLK;
  return true;
}

/// Lines over the Seed3's own USB-C port (CDC serial). Not libDaisy's Logger: its 128-byte
/// buffer truncates back-to-back lines. A line that cannot be sent within 20 ms (no terminal
/// open, or a stuck port) is dropped and counted rather than blocking the loop.
class UsbSerial {
 public:
  void init() {
    self_ = this;
    usb_.Init(::daisy::UsbHandle::FS_INTERNAL);
    usb_.SetReceiveCallback(&UsbSerial::onReceive, ::daisy::UsbHandle::FS_INTERNAL);
  }

  /// Call from the main loop. A "dfu" line from the host resets the chip into the STM32 ROM bootloader
  /// (BOOT0 high, then reset: System::ResetToBootloader), so the Mac can flash without button presses.
  /// Does not return when it fires.
  void serviceHost() {
    if (!dfuRequested_) return;
    line("# dfu: resetting into the ROM bootloader");
    ::daisy::System::Delay(50);  // let the line leave the USB FIFO
    ::daisy::System::ResetToBootloader(::daisy::System::BootloaderMode::STM);
  }

  void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list va;
    va_start(va, fmt);
    int n = std::vsnprintf(buf_, sizeof buf_ - 2, fmt, va);
    va_end(va);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf_) - 3) n = sizeof buf_ - 3;
    buf_[n++] = '\r';
    buf_[n++] = '\n';
    send(static_cast<std::size_t>(n));
  }

  std::uint32_t dropped() const { return dropped_; }

 private:
  void send(std::size_t n) {
    const std::uint32_t t0 = ::daisy::System::GetNow();
    while (usb_.TransmitInternal(reinterpret_cast<std::uint8_t*>(buf_), n) != ::daisy::UsbHandle::Result::OK) {
      if (::daisy::System::GetNow() - t0 > 20) {
        ++dropped_;
        return;
      }
    }
  }

  // Runs in the USB interrupt: only feeds the matcher and sets a flag.
  static void onReceive(std::uint8_t* buf, std::uint32_t* len) {
    if (self_ && self_->dfu_.feed(buf, *len)) self_->dfuRequested_ = true;
  }

  static inline UsbSerial* self_ = nullptr;
  ::daisy::UsbHandle usb_;
  DfuMatcher dfu_;
  volatile bool dfuRequested_ = false;
  char buf_[160];
  std::uint32_t dropped_ = 0;
};

/// The board after boot(): clock, budget and version, for the banner and the meter.
struct BoardInfo {
  std::uint32_t cpuHz = 0;
  std::uint32_t budget = 0;           // cycles per block of audio
  std::uint32_t blocksPerSecond = 0;  // callbacks per second
  float sampleRate = 0.0f;
  const char* version = "?";
};

inline const char* versionName(::daisy::DaisySeed::BoardVersion v) {
  using V = ::daisy::DaisySeed::BoardVersion;
  switch (v) {
    case V::DAISY_SEED_3: return "seed3";
    case V::DAISY_SEED_1_2: return "seed-1.2";
    case V::DAISY_SEED_2_DFM: return "seed2-dfm";
    case V::DAISY_SEED_1_1: return "seed-1.1";
    default: return "seed-rev4";
  }
}

/// 480 MHz (boost), 48 kHz, kBlock samples per callback, cycle counter on, USB serial up.
inline BoardInfo boot(::daisy::DaisySeed& hw, UsbSerial& serial) {
  hw.Init(true);
  hw.SetAudioSampleRate(::daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
  hw.SetAudioBlockSize(kBlock);
  Dwt::enable();
  serial.init();
  BoardInfo b;
  b.version = versionName(hw.CheckBoardVersion());
  b.cpuHz = ::daisy::System::GetSysClkFreq();
  b.sampleRate = hw.AudioSampleRate();
  b.budget = static_cast<std::uint32_t>(std::lround(static_cast<double>(b.cpuHz) * kBlock / b.sampleRate));
  b.blocksPerSecond = static_cast<std::uint32_t>(std::lround(b.sampleRate / kBlock));
  return b;
}

inline void banner(UsbSerial& s, const BoardInfo& b) {
  s.line("# %s block=%d rate=%lu cpu=%luHz budget=%lu cycles/block board=%s libDaisy=%s", SQUATCH_FIRMWARE, kBlock,
         static_cast<unsigned long>(std::lround(b.sampleRate)), static_cast<unsigned long>(b.cpuHz),
         static_cast<unsigned long>(b.budget), b.version, SQUATCH_LIBDAISY);
}

/// Peak level as "-12.3" dBFS (or "-inf"), without float printf.
inline const char* formatDb(char* buf, std::size_t size, float peak) {
  if (!(peak > 1e-6f)) return std::strncpy(buf, "-inf", size);
  const long tenths = std::lround(200.0f * std::log10(peak));
  const long mag = tenths < 0 ? -tenths : tenths;
  std::snprintf(buf, size, "%s%ld.%ld", tenths < 0 ? "-" : "", mag / 10, mag % 10);
  return buf;
}

/// The lines every firmware prints for a window: counts, then the callback and its period.
template <int Slots>
void reportCommon(UsbSerial& s, const Window<Slots>& w, std::uint32_t ungated) {
  char in[24], out[24], cb[80], period[80];
  s.line("w=%lu blocks=%lu late=%lu over=%lu stretched=%lu in=%s out=%s dBFS usb_ungate=%lu usb_dropped=%lu",
         static_cast<unsigned long>(w.seq), static_cast<unsigned long>(w.blocks), static_cast<unsigned long>(w.late),
         static_cast<unsigned long>(w.overruns), static_cast<unsigned long>(w.stretched),
         formatDb(in, sizeof in, w.inPeak), formatDb(out, sizeof out, w.outPeak), static_cast<unsigned long>(ungated),
         static_cast<unsigned long>(s.dropped()));
  formatSummary(cb, sizeof cb, "cpu", w.slot[0]);
  formatSummary(period, sizeof period, "period", w.slot[1]);
  s.line("w=%lu %s %s (%% of block: mean/p99/worst)", static_cast<unsigned long>(w.seq), cb, period);
}

/// Heartbeat: 1 Hz while everything is on time, 5 Hz once any callback has been late or overrun.
inline void heartbeat(::daisy::DaisySeed& hw, bool alarm) {
  const std::uint32_t ms = ::daisy::System::GetNow();
  hw.SetLed(((ms / (alarm ? 100u : 500u)) & 1u) != 0);
}

/// Absolute peak of n samples (the meter's in/out levels).
inline float peakOf(const float* x, std::size_t n) {
  float p = 0.0f;
  for (std::size_t i = 0; i < n; ++i) p = std::fmax(p, std::fabs(x[i]));
  return p;
}

}  // namespace squatch::board

// Any fault in any image ends in the ROM bootloader instead of a silent freeze (libDaisy's handlers execute a breakpoint,
// which with no debugger locks the core up). libDaisy defines HardFault_Handler itself, so the Makefile builds
// its startup file with that name changed (-DHardFault_Handler=SquatchHardFault_Handler): the vector table then points at
// the handler below. The other three replace libDaisy's weak ones.
extern "C" {
void SquatchHardFault_Handler() { squatch::board::resetToRomBootloader(); }
void MemManage_Handler() { squatch::board::resetToRomBootloader(); }
void BusFault_Handler() { squatch::board::resetToRomBootloader(); }
void UsageFault_Handler() { squatch::board::resetToRomBootloader(); }
}
