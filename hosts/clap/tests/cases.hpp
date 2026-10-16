// SPDX-License-Identifier: MIT
// The plugin requirement cases (research notes, frameworks review 2.1 and 4.4), each run on every
// plugin: from the linked-in entries under the sanitizers, or from built bundles (--bundle).
#pragma once
#include <clap/clap.h>

#include "host.hpp"

namespace cases {

struct Subject {
  const char* key;
  const clap_plugin_entry_t* entry;
};

// cases.cpp: the main-thread side
void descriptorAndParams(const Subject& s);
void stateBeforeActivation(const Subject& s);
void flushWhileInactive(const Subject& s);
void portConfigurations(const Subject& s);
void hostInformation(const Subject& s);

// cases_audio.cpp: processing
void blockSizesAndLayouts(const Subject& s);
void bypassKeepsProcessingAligned(const Subject& s);
void latencyAndSampleRate(const Subject& s);

// cases_threads.cpp: meters, and more than one thread
void metersReachTheHost(const Subject& s);
void mainThreadWhileProcessing(const Subject& s);
void fileSwapsOffThread(const Subject& s);

/// Cases for one plugin only, which a tests/extra_*.cpp file registers with a static Extra (by
/// the plugin's CLAP id); they run after the shared cases, on the linked plugin and its bundle.
struct Extra {
  const char* pluginId;
  void (*run)(const Subject& s);
  const Extra* next;
  Extra(const char* id, void (*fn)(const Subject&));
};
void extraCases(const Subject& s);

/// Calls process() inside the real-time watch (and RealtimeSanitizer's nonblocking scope when
/// built with it); adds what it saw to the running totals.
clap_process_status processWatched(const clap_plugin_t* p, const clap_process_t* process);
long rtViolations();
void resetRtViolations();

/// True when every sample is finite and not subnormal (clap-validator's own audio check).
bool clean(const float* x, uint32_t n);

}  // namespace cases
