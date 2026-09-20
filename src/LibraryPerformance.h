#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <string_view>
#include <vector>

struct LibraryBookWindow {
  size_t first = 0;
  size_t renderedEnd = 0;
  size_t titledEnd = 0;
};

inline LibraryBookWindow makeLibraryBookWindow(const size_t bookCount, const size_t scrollRow,
                                                const size_t visibleRows, const size_t columns) {
  const size_t first = std::min(scrollRow * columns, bookCount);
  return {first, std::min(first + (visibleRows + 1) * columns, bookCount),
          std::min(first + visibleRows * columns, bookCount)};
}

struct LibraryLookupStats {
  size_t probes = 0;
};

// A compact sorted view over a vector whose display order must not change. At the Library's
// 2,048-book limit this uses 4 KiB temporarily and replaces repeated path scans with logarithmic
// lookups. release() drops it before cover decoding needs contiguous heap.
template <typename Item, typename Key, typename Less = std::less<Key>>
class SortedLibraryLookup {
 public:
  using KeyFunction = Key (*)(const Item&);
  static constexpr size_t NOT_FOUND = std::numeric_limits<size_t>::max();

  void reset(const std::vector<Item>& items, KeyFunction keyOf) {
    items_ = &items;
    keyOf_ = keyOf;
    order_.clear();
    if (items.size() > std::numeric_limits<uint16_t>::max()) return;
    order_.resize(items.size());
    std::iota(order_.begin(), order_.end(), uint16_t{0});
    std::sort(order_.begin(), order_.end(), [&](const uint16_t left, const uint16_t right) {
      return less_(keyOf_(items[left]), keyOf_(items[right]));
    });
  }

  size_t findIndex(const Key& key, LibraryLookupStats* stats = nullptr) const {
    if (!items_ || !keyOf_) return NOT_FOUND;
    size_t first = 0;
    size_t last = order_.size();
    while (first < last) {
      const size_t mid = first + (last - first) / 2;
      if (stats) ++stats->probes;
      const Key candidate = keyOf_((*items_)[order_[mid]]);
      if (less_(candidate, key)) {
        first = mid + 1;
      } else if (less_(key, candidate)) {
        last = mid;
      } else {
        return order_[mid];
      }
    }
    return NOT_FOUND;
  }

  const Item* find(const Key& key, LibraryLookupStats* stats = nullptr) const {
    const size_t index = findIndex(key, stats);
    return index == NOT_FOUND ? nullptr : &(*items_)[index];
  }

  void noteAppendedItem() {
    if (!items_ || !keyOf_ || items_->empty() || items_->size() > std::numeric_limits<uint16_t>::max()) {
      order_.clear();
      return;
    }
    const auto index = static_cast<uint16_t>(items_->size() - 1);
    const Key key = keyOf_((*items_)[index]);
    const auto position =
        std::lower_bound(order_.begin(), order_.end(), key, [&](const uint16_t existing, const Key& candidate) {
          return less_(keyOf_((*items_)[existing]), candidate);
        });
    order_.insert(position, index);
  }

  void release() {
    std::vector<uint16_t>().swap(order_);
    items_ = nullptr;
    keyOf_ = nullptr;
  }

  size_t indexEntryCount() const { return order_.size(); }

 private:
  const std::vector<Item>* items_ = nullptr;
  KeyFunction keyOf_ = nullptr;
  std::vector<uint16_t> order_;
  Less less_{};
};

inline std::string_view libraryFolderPath(const std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

inline bool libraryScanIgnoresEntry(const std::string_view name) {
  return name.empty() || name.front() == '.' || name == "System Volume Information" || name == "dict" ||
         name == "XTCache";
}

struct LibraryFolderOrderStats {
  size_t comparisons = 0;
  size_t pathReads = 0;
};

template <typename Item, typename PathFunction>
std::vector<uint16_t> makeLibraryFolderOrder(const std::vector<Item>& items, PathFunction pathOf,
                                             LibraryFolderOrderStats* stats = nullptr) {
  std::vector<uint16_t> order;
  if (items.size() > std::numeric_limits<uint16_t>::max()) return order;
  order.resize(items.size());
  std::iota(order.begin(), order.end(), uint16_t{0});
  std::stable_sort(order.begin(), order.end(), [&](const uint16_t left, const uint16_t right) {
    if (stats) {
      ++stats->comparisons;
      stats->pathReads += 2;
    }
    return libraryFolderPath(pathOf(items[left])) < libraryFolderPath(pathOf(items[right]));
  });
  return order;
}

template <typename Progress, typename IsPending>
size_t findNextPendingLibraryProgress(const std::vector<Progress>& progress, size_t& cursor, IsPending&& isPending,
                                      size_t* inspected = nullptr) {
  while (cursor < progress.size()) {
    const size_t candidate = cursor++;
    if (inspected) ++*inspected;
    if (isPending(progress[candidate])) return candidate;
  }
  return progress.size();
}
