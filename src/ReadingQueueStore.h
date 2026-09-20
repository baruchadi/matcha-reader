#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include "ReadingQueue.h"

class ReadingQueueStore : public PersistableStore<ReadingQueueStore> {
 public:
  ReadingQueueStore() = default;

  static const char* getFilePath() { return "/.crosspoint/reading_queue.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  [[nodiscard]] const ReadingQueue& queue() const { return queue_; }
  ReadingQueue& queue() { return queue_; }
  void clear() { queue_.clear(); }

 private:
  ReadingQueue queue_;
};
