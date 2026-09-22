#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

#include "RecentBook.h"

namespace cover_thumbnail {

// True only for a complete 2:3 BMP at the exact size the e-ink card will draw.
bool heightValid(const std::string& path, int height);

}  // namespace cover_thumbnail

// One shared background implementation for every cover-led UI. Activities decide which visible
// book to prioritise; this class owns the expensive EPUB/XTC/manga decode and cancellation rules.
class CoverThumbnailWorker {
 public:
  struct Job {
    RecentBook book;
    int primaryHeight = 0;
    static constexpr int MAX_TARGET_HEIGHTS = 3;
    int targetHeights[MAX_TARGET_HEIGHTS] = {0, 0, 0};
    uint32_t fileSize = 0;
    uint32_t modifiedStamp = 0;
    bool fetchSeriesMetadata = false;

    void addTargetHeight(int height);
  };

  struct Result {
    bool pending = false;
    bool completed = false;
    bool hasPrimaryThumb = false;
    bool coverKnownAbsent = false;
    bool metadataRequested = false;
    bool metadataLoaded = false;
    bool coverAttempted = false;
    int primaryHeight = 0;
    RecentBook book;
    uint32_t fileSize = 0;
    uint32_t modifiedStamp = 0;
  };

  CoverThumbnailWorker() = default;
  ~CoverThumbnailWorker();
  CoverThumbnailWorker(const CoverThumbnailWorker&) = delete;
  CoverThumbnailWorker& operator=(const CoverThumbnailWorker&) = delete;

  bool start(const char* taskName = "CoverThumb");
  void stop();
  bool post(Job&& job);
  void requestCancel() { cancelRequested_ = true; }
  [[nodiscard]] bool busy() const { return busy_.load(std::memory_order_acquire); }
  [[nodiscard]] const Result* pendingResult() const;
  void discardResult();

 private:
  static constexpr uint32_t STACK_BYTES = 8192;

  TaskHandle_t task_ = nullptr;
  std::atomic<bool> exitRequested_{false};
  std::atomic<bool> exited_{false};
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancelRequested_{false};
  std::atomic<bool> cancelSeen_{false};
  Job job_;
  Result result_;

  static void taskTrampoline(void* context);
  static bool shouldCancel(void* context);
  void taskLoop();
  void runJob();
};
