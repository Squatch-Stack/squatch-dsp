// SPDX-License-Identifier: MIT
// Watches for real-time violations between arm() and disarm(): heap traffic through the global
// operator new/delete (replaced in rt_watch.cpp) and through malloc/free (the sanitizer
// allocator's hooks), and lock acquisitions (lockcount.c, macOS). Only the thread that called
// arm() is watched, so a plugin's loader thread may allocate while its audio thread is checked.
#pragma once

namespace rt {

struct Counts {
  long news = 0, deletes = 0, mallocs = 0, frees = 0, locks = 0;
  long total() const { return news + deletes + mallocs + frees + locks; }
};

void arm();
Counts disarm();
bool mallocMeasured();  // malloc hooks need the sanitizer allocator
bool locksMeasured();

}  // namespace rt
