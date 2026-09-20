#include "ReadingQueueStore.h"

#include <cstring>

namespace {
constexpr size_t MAX_PATH_LENGTH = 500;
}

void ReadingQueueStore::toJson(JsonDocument& doc) const {
  doc["version"] = 1;
  JsonArray paths = doc["paths"].to<JsonArray>();
  for (const auto& path : queue_.paths()) paths.add(path);
}

bool ReadingQueueStore::fromJson(JsonVariantConst doc) {
  queue_.clear();
  const JsonArrayConst paths = doc["paths"].as<JsonArrayConst>();
  for (const JsonVariantConst value : paths) {
    if (queue_.size() >= ReadingQueue::MAX_BOOKS) break;
    const char* path = value.as<const char*>();
    if (!path || !*path || strnlen(path, MAX_PATH_LENGTH + 1) > MAX_PATH_LENGTH) continue;
    queue_.add(path);  // also drops duplicate paths from a hand-edited/corrupt file
  }
  return true;
}
