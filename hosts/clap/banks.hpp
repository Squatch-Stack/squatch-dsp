// SPDX-License-Identifier: MIT
// Two banks of block instances and the thread that loads files into the idle one, so a model or
// impulse response changes while audio runs without the audio thread waiting, allocating or
// reading a file (framework requirement: swap models off-thread with a short crossfade).
//
// Ownership, and the only hand-offs:
// - activate() (main thread, audio stopped) prepares bank 0 synchronously and makes it active.
// - request() (main thread, active) hands a path to the loader thread. The loader claims a bank
//   the audio thread is not using (busy_ false, or the one it published itself and the audio
//   thread never took), loads and prepares it, and publishes its index in ready_.
// - beginBlock() (audio thread) takes ready_ with one atomic exchange, gives the new bank the
//   current parameter values, and crossfades from the old bank over kSwapMs; when the fade ends it
//   clears the old bank's busy_ flag, which is the loader's permission to reuse it.
// - quiesce() (main thread, from deactivate) cancels a pending load and waits for the loader.
// A block without a file never starts the thread and only ever uses bank 0.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine.hpp"
#include "traits.hpp"

namespace squatch::clap {

template <class B>
class Banks {
 public:
  using Traits = PluginTraits<B>;
  using BankT = Bank<B, typename Traits::File>;
  static constexpr bool kHasFile = Traits::kFileKey != nullptr;

  Banks() {
    for (auto& b : banks_) b.reset(new BankT());
    busy_[0] = true;
    if (kHasFile) loader_ = std::thread([this] { loop(); });
  }

  ~Banks() {
    if (!loader_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    loader_.join();
  }

  /// Main thread, audio stopped. `error` names a file that did not load (audio then passes).
  bool activate(const Activation& a, const std::vector<float>& values, const std::string& path, std::string& error) {
    quiesce();
    active_ = 0, fadingOut_ = -1;
    busy_[0] = true, busy_[1] = false;
    return load(*banks_[0], a, values, path, error);
  }

  /// Called on the loader thread after a file failed to load (a host's request_callback).
  void onError(void (*notify)(void*), void* ctx) { notify_ = notify, notifyCtx_ = ctx; }

  /// Main thread, active: load `path` in the background and swap to it when ready.
  void request(const std::string& path, const Activation& a, std::vector<float> values) {
    if (!kHasFile) return;
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = Job{path, a, std::move(values)};
    pending_ = true;
    wake_.notify_all();
  }

  /// Main thread: no load in flight or waiting when this returns.
  void quiesce() {
    if (!kHasFile) return;
    std::unique_lock<std::mutex> lock(mutex_);
    pending_ = false;
    cancel_ = true;
    idle_.wait(lock, [this] { return !working_; });
    cancel_ = false;
    ready_.store(-1);
  }

  /// Audio thread, before processing: finish a crossfade, or start one to a freshly loaded bank.
  template <class Apply>
  void beginBlock(Engine& engine, Apply&& apply) {
    if (fadingOut_ >= 0 && !engine.swapping()) busy_[fadingOut_].store(false, std::memory_order_release), fadingOut_ = -1;
    if (fadingOut_ >= 0 || ready_.load(std::memory_order_relaxed) < 0) return;
    const int next = ready_.exchange(-1, std::memory_order_acq_rel);
    if (next < 0) return;
    busy_[next].store(true, std::memory_order_relaxed);
    apply(*banks_[next]);
    fadingOut_ = active_;
    active_ = next;
    engine.startSwap();
  }

  BankT& active() { return *banks_[active_]; }
  BankT* fadingOut() { return fadingOut_ >= 0 ? banks_[fadingOut_].get() : nullptr; }
  /// Loader thread's last error, for the main thread (empty: none).
  std::string takeError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(error_, std::string());
  }
  int swaps() const { return swaps_.load(); }

 private:
  struct Job {
    std::string path;
    Activation activation;
    std::vector<float> values;
  };

  // False only when memory ran out. A file that does not read leaves `error` set and the bank
  // passing audio through; the loader then keeps the current bank rather than swap to that.
  static bool load(BankT& bank, const Activation& a, const std::vector<float>& values, const std::string& path,
                   std::string& error) {
    Traits::load(path, a.sampleRate, bank.file, error);
    const auto configure = [&](B& block, const typename Traits::File& file) {
      Traits::configure(block, file);
      for (const dsp::ParamDesc& p : B::params())
        if (p.applied()) block.set(p.id, values[p.id]);
    };
    return bank.prepare(a, configure);
  }

  void loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || pending_; });
      if (stop_) return;
      Job job = std::move(job_);
      pending_ = false, working_ = true;
      lock.unlock();
      std::string error;
      const int slot = work(job, error);
      lock.lock();
      finish(slot, error);
    }
  }

  // Outside the lock: the slot now holding the job's file, or -1 (cancelled, or it failed).
  int work(const Job& job, std::string& error) {
    const int slot = claim();
    const bool ok = slot >= 0 && load(*banks_[slot], job.activation, job.values, job.path, error);
    return ok && error.empty() ? slot : -1;
  }

  // Under the lock: publish the bank, or the error.
  void finish(int slot, const std::string& error) {
    if (slot >= 0 && !cancel_) ready_.store(slot, std::memory_order_release), swaps_.fetch_add(1);
    if (!error.empty()) error_ = error;
    working_ = false;
    idle_.notify_all();
    if (!error.empty() && notify_) notify_(notifyCtx_);
  }

  // A bank the audio thread does not use: one published but never taken, or one not busy.
  int claim() {
    const int unclaimed = ready_.exchange(-1, std::memory_order_acq_rel);
    if (unclaimed >= 0) return unclaimed;
    while (!stopping()) {
      for (int i = 0; i < 2; ++i)
        if (!busy_[i].load(std::memory_order_acquire)) return i;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return -1;
  }

  bool stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_ || cancel_;
  }

  std::unique_ptr<BankT> banks_[2];
  int active_ = 0, fadingOut_ = -1;  // audio thread (main thread while inactive)
  std::atomic<bool> busy_[2] = {};
  std::atomic<int> ready_{-1};
  std::atomic<int> swaps_{0};  // banks published, for tests

  std::mutex mutex_;
  std::condition_variable wake_, idle_;
  Job job_;
  std::string error_;
  bool pending_ = false, working_ = false, stop_ = false, cancel_ = false;
  void (*notify_)(void*) = nullptr;
  void* notifyCtx_ = nullptr;
  std::thread loader_;
};

}  // namespace squatch::clap
