#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "ReaderPerformance.h"

// Keeps noncritical recents/statistics SD writes off the tap-to-first-page path. The activity
// task owns pending_; the render task publishes first-frame completion through atomics.
class DeferredReaderOpenBookkeeping {
 public:
  void arm() {
    firstRenderComplete_.store(false, std::memory_order_relaxed);
    firstRenderCompleteAtMs_.store(0, std::memory_order_relaxed);
    pending_ = true;
  }

  void noteRenderResult(const bool userFrameDisplayed, const uint32_t completedAtMs) {
    if (!userFrameDisplayed) return;
    firstRenderCompleteAtMs_.store(completedAtMs, std::memory_order_relaxed);
    firstRenderComplete_.store(true, std::memory_order_release);
  }

  template <typename Work>
  bool runIfReadyWhenIdle(const uint32_t nowMs, const bool foregroundInputPending, Work&& work) {
    if (!pending_ || foregroundInputPending || !firstRenderComplete_.load(std::memory_order_acquire)) return false;
    const uint32_t completedAtMs = firstRenderCompleteAtMs_.load(std::memory_order_relaxed);
    if (nowMs - completedAtMs < READER_OPEN_BOOKKEEPING_IDLE_MS) return false;
    pending_ = false;
    std::forward<Work>(work)();
    return true;
  }

  template <typename Work>
  bool runNowIfPending(Work&& work) {
    if (!pending_) return false;
    pending_ = false;
    std::forward<Work>(work)();
    return true;
  }

 private:
  std::atomic<bool> firstRenderComplete_{false};
  std::atomic<uint32_t> firstRenderCompleteAtMs_{0};
  bool pending_ = false;
};
