// SPDX-License-Identifier: MIT
// Plumbing every live host shares, JACK or CoreAudio: the run flag a signal clears, the clocks, and a
// sleep to an absolute time. Nothing here runs in an audio callback except nowNs(), which is
// clock_gettime (no lock, no allocation; on Linux a vDSO call, on macOS a cheap system call).
#pragma once
#include <atomic>
#include <csignal>
#include <cstdint>
#include <ctime>

namespace squatch::host {

inline std::atomic<bool>& running() {
  static std::atomic<bool> flag{true};
  return flag;
}

inline uint64_t nowNs(clockid_t clock = CLOCK_MONOTONIC) {
  timespec t;
  clock_gettime(clock, &t);
  return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + static_cast<uint64_t>(t.tv_nsec);
}

inline void onStopSignal(int) { running().store(false); }

/// SIGINT and SIGTERM stop the program cleanly; so does the reader of stdout going away (SIGPIPE).
inline void installStopSignals() {
  std::signal(SIGINT, onStopSignal);
  std::signal(SIGTERM, onStopSignal);
  std::signal(SIGPIPE, onStopSignal);
}

/// Sleep until an absolute CLOCK_MONOTONIC time, then advance it by `periodNs`.
/// macOS has no clock_nanosleep, so there it sleeps for the time left, read from the same clock.
inline void sleepUntil(uint64_t& deadlineNs, uint64_t periodNs) {
  deadlineNs += periodNs;
#if defined(__APPLE__)
  for (;;) {
    const uint64_t now = nowNs();
    if (now >= deadlineNs) return;
    const uint64_t left = deadlineNs - now;
    timespec t;
    t.tv_sec = static_cast<time_t>(left / 1000000000ull);
    t.tv_nsec = static_cast<long>(left % 1000000000ull);
    if (nanosleep(&t, nullptr) == 0) return;  // interrupted by a signal: go round with the time still left
  }
#else
  timespec t;
  t.tv_sec = static_cast<time_t>(deadlineNs / 1000000000ull);
  t.tv_nsec = static_cast<long>(deadlineNs % 1000000000ull);
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr);
#endif
}

}  // namespace squatch::host
