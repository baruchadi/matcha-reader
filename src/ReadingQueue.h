#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Small, ordered path-only model for the reading queue. Metadata stays in the library catalog,
// so adding a book here does not duplicate its title/author/cover allocations. The hard cap keeps
// the feature predictable on the X3's 380KB DRAM ceiling.
class ReadingQueue {
 public:
  static constexpr size_t MAX_BOOKS = 32;

  [[nodiscard]] const std::vector<std::string>& paths() const { return paths_; }
  [[nodiscard]] size_t size() const { return paths_.size(); }
  [[nodiscard]] bool empty() const { return paths_.empty(); }

  void clear() { paths_.clear(); }

  [[nodiscard]] int indexOf(const std::string& path) const {
    const auto it = std::find(paths_.begin(), paths_.end(), path);
    return it == paths_.end() ? -1 : static_cast<int>(it - paths_.begin());
  }

  [[nodiscard]] bool contains(const std::string& path) const { return indexOf(path) >= 0; }

  bool add(const std::string& path) {
    if (path.empty() || contains(path) || paths_.size() >= MAX_BOOKS) return false;
    paths_.push_back(path);
    return true;
  }

  bool remove(const std::string& path) {
    const auto it = std::find(paths_.begin(), paths_.end(), path);
    if (it == paths_.end()) return false;
    paths_.erase(it);
    return true;
  }

  bool moveEarlier(const std::string& path) {
    const int index = indexOf(path);
    if (index <= 0) return false;
    std::swap(paths_[index], paths_[index - 1]);
    return true;
  }

  bool moveLater(const std::string& path) {
    const int index = indexOf(path);
    if (index < 0 || index + 1 >= static_cast<int>(paths_.size())) return false;
    std::swap(paths_[index], paths_[index + 1]);
    return true;
  }

  bool updatePath(const std::string& oldPath, const std::string& newPath) {
    const int oldIndex = indexOf(oldPath);
    if (oldIndex < 0 || newPath.empty() || oldPath == newPath) return false;
    const int newIndex = indexOf(newPath);
    if (newIndex >= 0) {
      paths_.erase(paths_.begin() + oldIndex);
    } else {
      paths_[oldIndex] = newPath;
    }
    return true;
  }

 private:
  std::vector<std::string> paths_;
};
