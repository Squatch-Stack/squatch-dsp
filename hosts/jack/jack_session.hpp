// SPDX-License-Identifier: MIT
// Plumbing shared by the JACK programs here: open a client, stop cleanly on a signal or when
// jackd goes away, and small time helpers. Nothing in this file runs in the audio callback
// except nowNs(), which is clock_gettime on the vDSO (no system call, no lock).
#pragma once
#include <jack/jack.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
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
inline void onJackShutdown(void*) { running().store(false); }

/// Connect to a running jackd (never start one) and arrange a clean stop. Null on failure.
inline jack_client_t* openClient(const char* name) {
  jack_status_t status;
  jack_client_t* c = jack_client_open(name, JackNoStartServer, &status);
  if (!c) {
    std::fprintf(stderr, "%s: cannot connect to JACK (status 0x%x) - is jackd running?\n", name, status);
    return nullptr;
  }
  jack_on_shutdown(c, onJackShutdown, nullptr);
  std::signal(SIGINT, onStopSignal);
  std::signal(SIGTERM, onStopSignal);
  std::signal(SIGPIPE, onStopSignal);  // the reader of our stdout went away
  return c;
}

/// Connect two ports by name; report (not fail) when one is missing.
inline bool connectPorts(jack_client_t* c, const char* from, const char* to) {
  const int rc = jack_connect(c, from, to);
  if (rc != 0 && rc != EEXIST) std::fprintf(stderr, "cannot connect %s -> %s\n", from, to);
  return rc == 0 || rc == EEXIST;
}

/// Sleep until an absolute CLOCK_MONOTONIC time, then advance it by `periodNs`.
/// macOS has no clock_nanosleep, so there it sleeps for the time left, read from the same clock.
inline void sleepUntil(uint64_t& deadlineNs, uint64_t periodNs) {
  deadlineNs += periodNs;
#if defined(__APPLE__)
  for (;;) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t nowNs = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
    if (nowNs >= deadlineNs) return;
    const uint64_t left = deadlineNs - nowNs;
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
