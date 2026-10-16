// SPDX-License-Identifier: MIT
// squatch-clap-host: the plugin requirement cases, headless.
//   squatch-clap-host                   every plugin linked into this binary (the sanitizer legs)
//   squatch-clap-host --only KEY        one of them
//   squatch-clap-host --bundle X.clap   a built bundle, loaded as a host would (dlopen)
//   squatch-clap-host --prove-watch     fail first: a bad process() the watch (and RTSan) must catch
// The allocation and lock watch covers every process() call; built with -fsanitize=realtime,
// each call also runs in RealtimeSanitizer's nonblocking scope, which reaches into bundles too.
#include <dlfcn.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "cases.hpp"
#include "rt_watch.hpp"

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#define SQUATCH_RTSAN 1
#endif
#if __has_feature(thread_sanitizer)
#define SQUATCH_TSAN 1  // TSan's own bookkeeping takes locks: the lock watch means nothing there
#endif
#endif

// Every plugin CMake linked in (generated/linked_plugins.inc: SQUATCH_LINKED(key) per plugin).
#define SQUATCH_LINKED(KEY) extern "C" const clap_plugin_entry_t squatch_clap_entry_##KEY;
#include "linked_plugins.inc"
#undef SQUATCH_LINKED

namespace cases {
namespace {

long gViolations = 0;

#ifdef SQUATCH_RTSAN
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
clap_process_status nonblocking(const clap_plugin_t* p, const clap_process_t* process) [[clang::nonblocking]] {
  return p->process(p, process);
}
#pragma clang diagnostic pop
#else
clap_process_status nonblocking(const clap_plugin_t* p, const clap_process_t* process) { return p->process(p, process); }
#endif

void suite(const Subject& s) {
  const int before = host::tally().failures;
  resetRtViolations();
  descriptorAndParams(s);
  stateBeforeActivation(s);
  flushWhileInactive(s);
  portConfigurations(s);
  hostInformation(s);
  blockSizesAndLayouts(s);
  bypassKeepsProcessingAligned(s);
  latencyAndSampleRate(s);
  metersReachTheHost(s);
  mainThreadWhileProcessing(s);
  fileSwapsOffThread(s);
  extraCases(s);
#ifndef SQUATCH_TSAN
  HCHECK(rtViolations() == 0, "%s: %ld allocations, frees or locks inside process()", s.key, rtViolations());
#endif
  std::printf("%s %s\n", host::tally().failures == before ? "ok  " : "FAIL", s.key);
}

}  // namespace

clap_process_status processWatched(const clap_plugin_t* p, const clap_process_t* process) {
  rt::arm();
  const clap_process_status status = nonblocking(p, process);
  const rt::Counts c = rt::disarm();
  if (c.total() && !gViolations)
    std::printf("     first violation: %ld new, %ld delete, %ld malloc, %ld free, %ld lock (%u frames)\n", c.news, c.deletes,
                c.mallocs, c.frees, c.locks, process->frames_count);
  gViolations += c.total();
  return status;
}

long rtViolations() { return gViolations; }

Extra* gExtras = nullptr;

Extra::Extra(const char* id, void (*fn)(const Subject&)) : pluginId(id), run(fn), next(gExtras) { gExtras = this; }

void extraCases(const Subject& s) {
  const auto* f = static_cast<const clap_plugin_factory_t*>(s.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* d = f ? f->get_plugin_descriptor(f, 0) : nullptr;
  for (const Extra* e = gExtras; d && e; e = e->next)
    if (std::strcmp(e->pluginId, d->id) == 0) e->run(s);
}
void resetRtViolations() { gViolations = 0; }

bool clean(const float* x, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (!std::isfinite(x[i]) || (x[i] != 0.0f && std::fabs(x[i]) < 1.17549435e-38f)) return false;
  return true;
}

}  // namespace cases

namespace {

// A bundle's binary: X.clap/Contents/MacOS/X on macOS, the file itself elsewhere.
std::string binaryOf(const std::string& bundle) {
#ifdef __APPLE__
  const std::string name = bundle.substr(bundle.find_last_of('/') + 1);
  return bundle + "/Contents/MacOS/" + name.substr(0, name.rfind(".clap"));
#else
  return bundle;
#endif
}

// Fail first: a process() that allocates and locks must be seen by the watch, and must make
// RealtimeSanitizer abort (make plugins-check expects that exit and its report).
int proveWatch() {
  static std::mutex m;
  clap_plugin_t bad{};
  bad.process = [](const clap_plugin_t*, const clap_process_t*) -> clap_process_status {
    std::lock_guard<std::mutex> hold(m);
    int* volatile p = new int(1);
    delete p;
    return CLAP_PROCESS_CONTINUE;
  };
  clap_process_t process{};
  cases::resetRtViolations();
  cases::processWatched(&bad, &process);
  std::printf("watch saw %ld violations in a bad process()\n", cases::rtViolations());
  return cases::rtViolations() >= 2 ? 0 : 1;
}

int runBundle(const std::string& bundle) {
  void* lib = dlopen(binaryOf(bundle).c_str(), RTLD_NOW | RTLD_LOCAL);
  const auto* entry = lib ? static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry")) : nullptr;
  if (!entry || !entry->init(bundle.c_str())) return std::printf("cannot load %s: %s\n", bundle.c_str(), dlerror()), 2;
  const auto* f = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  const char* id = f->get_plugin_descriptor(f, 0)->id;
  const std::string key = std::string(id).substr(std::string(id).rfind('.') + 1);
  cases::suite({key.c_str(), entry});
  entry->deinit();
  return 0;
}

void runLinked(const char* only) {
#define SQUATCH_LINKED(KEY) {#KEY, &squatch_clap_entry_##KEY},
  const cases::Subject linked[] = {
#include "linked_plugins.inc"
  };
#undef SQUATCH_LINKED
  for (const cases::Subject& s : linked)
    if (!only || std::strcmp(only, s.key) == 0) cases::suite(s);
}

const char* sanitizer() {
#if defined(SQUATCH_RTSAN)
  return "; RealtimeSanitizer on";
#elif defined(SQUATCH_TSAN)
  return "; ThreadSanitizer on (the watch is not checked under it)";
#else
  return "";
#endif
}

int report() {
  std::printf("clap host: %d checks, %d failure(s); rt watch: malloc %s, locks %s%s\n", host::tally().checks,
              host::tally().failures, rt::mallocMeasured() ? "measured" : "not measured (no ASan)",
              rt::locksMeasured() ? "measured" : "not measured", sanitizer());
  return host::tally().failures == 0 && host::tally().checks > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "--prove-watch") return proveWatch();
  for (int i = 2; mode == "--bundle" && i < argc; ++i)
    if (runBundle(argv[i]) != 0) return 2;
  if (mode != "--bundle") runLinked(mode == "--only" && argc > 2 ? argv[2] : nullptr);
  return report();
}
