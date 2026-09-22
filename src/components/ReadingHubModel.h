#pragma once

#include <cstdint>

namespace reading_hub {

enum class Section : uint8_t { NOW = 0, LIBRARY = 1, QUEUE = 2, READ = 3 };
enum class Page : uint8_t { ROOT = 0, SHELF_BOOKS = 1, ALL_SHELVES = 2, COMPLETED_BOOKS = 3 };

}  // namespace reading_hub
