#include "CoverThumbnailWorker.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaPanel.h>
#include <Xtc.h>

#include <algorithm>
#include <utility>

#include "SeriesMetadata.h"

namespace {
constexpr int COVER_ASPECT_NUM = 2;
constexpr int COVER_ASPECT_DEN = 3;
}

namespace cover_thumbnail {

bool heightValid(const std::string& path, const int height) {
  if (path.empty() || height <= 0) return false;
  if (!FsHelpers::hasCompleteBmp("COVER", path)) return false;
  const int expectedWidth = height * COVER_ASPECT_NUM / COVER_ASPECT_DEN;
  HalFile file;
  if (!Storage.openFileForRead("COVER", path, file)) return false;
  Bitmap bitmap(file);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() == height &&
                     bitmap.getWidth() == expectedWidth;
  file.close();
  return valid;
}

}  // namespace cover_thumbnail

namespace {

// Every decoder has historically used a slightly different cache-existence guard. Remove any
// present artifact that is not the exact, complete 2:3 target before asking a decoder to rebuild
// it; otherwise XTC/manga (and dimensionally stale EPUB thumbs) can mistake it for finished work.
bool needsRegeneration(const std::string& path, const int height) {
  if (cover_thumbnail::heightValid(path, height)) return false;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  return true;
}

}  // namespace

void CoverThumbnailWorker::Job::addTargetHeight(const int height) {
  if (height <= 0) return;
  for (int& slot : targetHeights) {
    if (slot == height) return;
    if (slot == 0) {
      slot = height;
      return;
    }
  }
}

CoverThumbnailWorker::~CoverThumbnailWorker() { stop(); }

bool CoverThumbnailWorker::start(const char* taskName) {
  if (task_) return true;
  exitRequested_ = false;
  exited_ = false;
  busy_.store(false, std::memory_order_relaxed);
  cancelRequested_ = false;
  cancelSeen_ = false;
  result_ = Result{};
  if (xTaskCreate(&taskTrampoline, taskName, STACK_BYTES, this, 1, &task_) == pdPASS) return true;
  task_ = nullptr;
  LOG_ERR("COVER", "Failed to create thumbnail worker");
  return false;
}

void CoverThumbnailWorker::stop() {
  if (!task_) return;
  exitRequested_ = true;
  cancelRequested_ = true;
  xTaskNotifyGive(task_);
  while (!exited_) vTaskDelay(1);
  task_ = nullptr;
}

bool CoverThumbnailWorker::post(Job&& job) {
  if (!task_ || busy() || result_.pending) return false;
  if (job.targetHeights[0] == 0 && !job.fetchSeriesMetadata) job.addTargetHeight(job.primaryHeight);
  job_ = std::move(job);
  result_ = Result{};
  cancelRequested_ = false;
  cancelSeen_ = false;
  busy_.store(true, std::memory_order_release);
  xTaskNotifyGive(task_);
  return true;
}

const CoverThumbnailWorker::Result* CoverThumbnailWorker::pendingResult() const {
  return !busy() && result_.pending ? &result_ : nullptr;
}

void CoverThumbnailWorker::discardResult() { result_.pending = false; }

void CoverThumbnailWorker::taskTrampoline(void* context) {
  static_cast<CoverThumbnailWorker*>(context)->taskLoop();
}

bool CoverThumbnailWorker::shouldCancel(void* context) {
  auto* worker = static_cast<CoverThumbnailWorker*>(context);
  if (!worker) return true;
  const bool cancel = worker->exitRequested_ || worker->cancelRequested_;
  if (cancel) worker->cancelSeen_ = true;
  return cancel;
}

void CoverThumbnailWorker::taskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (exitRequested_) break;
    if (!busy()) continue;
    runJob();
    busy_.store(false, std::memory_order_release);
  }
  exited_ = true;
  vTaskDelete(nullptr);
}

void CoverThumbnailWorker::runJob() {
  Result output;
  output.book = job_.book;
  output.fileSize = job_.fileSize;
  output.modifiedStamp = job_.modifiedStamp;
  output.metadataRequested = job_.fetchSeriesMetadata;
  output.coverAttempted = job_.targetHeights[0] > 0;
  output.primaryHeight = job_.primaryHeight;

  const bool epubBook = FsHelpers::hasEpubExtension(output.book.path);
  const bool xtcBook = FsHelpers::hasXtcExtension(output.book.path);
  if (!epubBook && !xtcBook) {
    const manga::MangaBook manga(output.book.path);
    for (const int height : job_.targetHeights) {
      if (height <= 0 || shouldCancel(this)) continue;
      const std::string thumb = manga.getThumbBmpPath(height);
      if (!needsRegeneration(thumb, height)) continue;
      manga.generateThumbBmp(height, &shouldCancel, this);
    }
    output.hasPrimaryThumb =
        cover_thumbnail::heightValid(manga.getThumbBmpPath(job_.primaryHeight), job_.primaryHeight);
    if (output.hasPrimaryThumb) output.book.coverBmpPath = manga.getThumbBmpPath();
  } else if (epubBook) {
    Epub epub(output.book.path, "/.crosspoint");
    if (job_.fetchSeriesMetadata) {
      std::string title;
      std::string author;
      std::string series;
      std::string seriesIndex;
      if (epub.loadMetadata(title, author, series, seriesIndex)) {
        if (!title.empty()) output.book.title = std::move(title);
        if (!author.empty()) output.book.author = std::move(author);
        output.book.series = std::move(series);
        output.book.seriesPosition = series_metadata::parsePosition(seriesIndex);
        output.book.seriesMetadataScanned = true;
        output.metadataLoaded = true;
      }
    }
    const bool loaded = !output.coverAttempted || epub.load(true, true, &shouldCancel, this);
    for (const int height : job_.targetHeights) {
      if (height <= 0) continue;
      if (!loaded || shouldCancel(this)) break;
      if (needsRegeneration(epub.getThumbBmpPath(height), height)) {
        epub.generateThumbBmp(height, &shouldCancel, this);
      }
    }
    output.hasPrimaryThumb = output.coverAttempted && loaded &&
                             cover_thumbnail::heightValid(epub.getThumbBmpPath(job_.primaryHeight), job_.primaryHeight);
    if (output.hasPrimaryThumb) output.book.coverBmpPath = epub.getThumbBmpPath();
    if (loaded && !epub.getTitle().empty()) output.book.title = epub.getTitle();
    output.coverKnownAbsent = output.coverAttempted && loaded && !output.hasPrimaryThumb && !shouldCancel(this) &&
                              (!epub.hasCoverImage() || epub.coverUnsupported());
    if (output.coverAttempted && loaded && !output.hasPrimaryThumb && !output.coverKnownAbsent) {
      LOG_ERR("COVER", "Thumbnail failed for %s; will retry", output.book.path.c_str());
    }
  } else {
    Xtc xtc(output.book.path, "/.crosspoint");
    const bool loaded = xtc.load();
    for (const int height : job_.targetHeights) {
      if (height <= 0) continue;
      if (!loaded || shouldCancel(this)) break;
      if (needsRegeneration(xtc.getThumbBmpPath(height), height)) {
        xtc.generateThumbBmp(height, &shouldCancel, this);
      }
    }
    output.hasPrimaryThumb = loaded &&
                             cover_thumbnail::heightValid(xtc.getThumbBmpPath(job_.primaryHeight), job_.primaryHeight);
    if (output.hasPrimaryThumb) output.book.coverBmpPath = xtc.getThumbBmpPath();
    if (loaded && !xtc.getTitle().empty()) output.book.title = xtc.getTitle();
  }

  output.completed = !cancelSeen_ && !exitRequested_;
  output.pending = true;
  result_ = std::move(output);
}
