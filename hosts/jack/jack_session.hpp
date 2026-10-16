// SPDX-License-Identifier: MIT
// Plumbing shared by the JACK programs here: open a client, stop cleanly on a signal or when
// jackd goes away. The run flag, clocks and sleep are in ../common/host_clock.hpp.
#pragma once
#include <jack/jack.h>

#include <cerrno>
#include <cstdio>

#include "../common/host_clock.hpp"

namespace squatch::host {

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
  installStopSignals();
  return c;
}

/// Connect two ports by name; report (not fail) when one is missing.
inline bool connectPorts(jack_client_t* c, const char* from, const char* to) {
  const int rc = jack_connect(c, from, to);
  if (rc != 0 && rc != EEXIST) std::fprintf(stderr, "cannot connect %s -> %s\n", from, to);
  return rc == 0 || rc == EEXIST;
}

}  // namespace squatch::host
