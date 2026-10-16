// SPDX-License-Identifier: MIT
// Counts lock acquisitions in the test process (macOS). Linked as a dylib, whose __interpose
// section dyld applies to every image: calls from libc++ (std::mutex) are counted too. Only the
// watched thread's locks count (lockcount_watch; a null thread watches none).
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>

static _Atomic long gLocks;
static _Atomic(pthread_t) gWatched;

long lockcount_get(void) { return atomic_load(&gLocks); }
void lockcount_watch(pthread_t thread) { atomic_store(&gWatched, thread); }

static void counted(void) {
  if (pthread_equal(pthread_self(), atomic_load(&gWatched))) atomic_fetch_add(&gLocks, 1);
}

static int countedLock(pthread_mutex_t* m) {
  counted();
  return pthread_mutex_lock(m);
}

static int countedTryLock(pthread_mutex_t* m) {
  counted();
  return pthread_mutex_trylock(m);
}

static void countedUnfairLock(os_unfair_lock_t l) {
  counted();
  os_unfair_lock_lock(l);
}

static int countedRdLock(pthread_rwlock_t* l) {
  counted();
  return pthread_rwlock_rdlock(l);
}

static int countedWrLock(pthread_rwlock_t* l) {
  counted();
  return pthread_rwlock_wrlock(l);
}

struct Interpose {
  const void* replacement;
  const void* original;
};

__attribute__((used)) static const struct Interpose kInterpose[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void*)&countedLock, (const void*)&pthread_mutex_lock},
    {(const void*)&countedTryLock, (const void*)&pthread_mutex_trylock},
    {(const void*)&countedUnfairLock, (const void*)&os_unfair_lock_lock},
    {(const void*)&countedRdLock, (const void*)&pthread_rwlock_rdlock},
    {(const void*)&countedWrLock, (const void*)&pthread_rwlock_wrlock},
};
