// SPDX-License-Identifier: MIT
#include "rt_watch.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#include <pthread.h>

#if defined(__SANITIZE_ADDRESS__)  // GCC
#define SQUATCH_MALLOC_HOOKS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SQUATCH_MALLOC_HOOKS 1
#endif
#endif
#ifdef SQUATCH_MALLOC_HOOKS
// Declared here rather than from <sanitizer/allocator_interface.h>, which GCC does not install;
// both sanitizer runtimes (compiler-rt and GCC's libsanitizer) provide it.
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*onMalloc)(const volatile void*, std::size_t),
                                                         void (*onFree)(const volatile void*));
#endif

#ifndef SQUATCH_NO_LOCK_COUNT
extern "C" long lockcount_get(void);
extern "C" void lockcount_watch(pthread_t thread);
#endif

namespace {

std::atomic<bool> gArmed{false};
std::atomic<pthread_t> gWatched{};  // pthread_self() never allocates, unlike first use of a thread_local
std::atomic<long> gNews{0}, gDeletes{0}, gMallocs{0}, gFrees{0};
long gLocksAtArm = 0;

void count(std::atomic<long>& c) {
  if (gArmed.load(std::memory_order_relaxed) && pthread_equal(pthread_self(), gWatched.load(std::memory_order_relaxed)))
    c.fetch_add(1, std::memory_order_relaxed);
}

long locksNow() {
#ifndef SQUATCH_NO_LOCK_COUNT
  return lockcount_get();
#else
  return 0;
#endif
}

#ifdef SQUATCH_MALLOC_HOOKS
void onMalloc(const volatile void*, std::size_t) { count(gMallocs); }
void onFree(const volatile void*) { count(gFrees); }
const bool gHooked = __sanitizer_install_malloc_and_free_hooks(onMalloc, onFree) != 0;
#endif

void* allocate(std::size_t n) {
  count(gNews);
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();  // no exceptions in this build
  return p;
}

void release(void* p) {
  if (p) count(gDeletes);
  std::free(p);
}

}  // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }

namespace rt {

void arm() {
  gNews = gDeletes = gMallocs = gFrees = 0;
  gWatched = pthread_self();
#ifndef SQUATCH_NO_LOCK_COUNT
  lockcount_watch(pthread_self());
#endif
  gLocksAtArm = locksNow();
  gArmed = true;
}

Counts disarm() {
  gArmed = false;
  gWatched = pthread_t{};  // no thread is watched between windows, so none can race into the next
#ifndef SQUATCH_NO_LOCK_COUNT
  lockcount_watch(pthread_t{});
#endif
  Counts c;
  c.news = gNews;
  c.deletes = gDeletes;
  c.mallocs = gMallocs;
  c.frees = gFrees;
  c.locks = locksNow() - gLocksAtArm;
  return c;
}

bool mallocMeasured() {
#ifdef SQUATCH_MALLOC_HOOKS
  return gHooked;
#else
  return false;
#endif
}

bool locksMeasured() {
#ifndef SQUATCH_NO_LOCK_COUNT
  return true;
#else
  return false;
#endif
}

}  // namespace rt
