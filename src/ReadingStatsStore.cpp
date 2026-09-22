#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <LibraryFormat.h>
#include <Logging.h>
#include <Memory.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <cstring>
#include <numeric>

ReadingStatsStore ReadingStatsStore::instance;

static std::string statsPath() { return sdsystem::path("reading_stats.bin"); }
// v3 appends the per-book block after the finished-book paths; v4 appends the per-day-per-language
// block after that; v5 appends one rating byte per finished path. Each older reader stops where
// its own format ends and ignores what follows, rather than rejecting the file -- it will,
// however, drop the newer blocks the next time it saves.
static constexpr uint8_t STATS_VERSION = 5;
// Longest language tag a per-book entry stores, on disk and in memory. Enforced on BOTH sides:
// the writer clamps to it, and the loader rejects anything longer rather than allocating on a
// corrupt or misaligned file's say-so.
static constexpr size_t MAX_STORED_LANGUAGE = 15;

namespace {
int daysSinceEpoch(uint16_t y, uint8_t m, uint8_t d) {
  int yy = y, mm = m;
  if (mm <= 2) {
    yy--;
    mm += 12;
  }
  return 365 * yy + yy / 4 - yy / 100 + yy / 400 + (153 * (mm - 3) + 2) / 5 + d - 306;
}

int dowFromDate(uint16_t y, uint8_t m, uint8_t d) {
  return (daysSinceEpoch(y, m, d) + 1) % 7;  // 0=Sun
}

void subtractDays(uint16_t& y, uint8_t& m, uint8_t& d, int n) {
  // Exact Gregorian inverse of daysSinceEpoch (Howard Hinnant's civil_from_days, shifted so
  // day 0 = 0000-03-01). The previous version estimated the year with the Julian 1461-day
  // cycle, which by the 2020s runs ~15 days late -- dates in the first half of March resolved
  // into the previous March-based year and came back 1-2 days off, corrupting streaks and
  // week/month views that cross early March.
  const int z = daysSinceEpoch(y, m, d) - n + 305;                             // days since 0000-03-01
  const int era = (z >= 0 ? z : z - 146096) / 146097;                          // 400-year eras
  const unsigned doe = static_cast<unsigned>(z - era * 146097);                // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                     // [0, 11], 0 = March
  d = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<uint16_t>(static_cast<int>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

// "ja-JP" / "JA" / "ja_jp" all bucket as "ja". Anything longer than 3 chars (no ISO-639 code is)
// is truncated rather than rejected, so a malformed tag still lands in a stable bucket.
void normalizeLanguage(const char* in, char out[4]) {
  size_t n = 0;
  for (const char* p = in; p && *p; p++) {
    const char c = *p;
    if (c == '-' || c == '_' || n == 3) break;
    out[n++] = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  }
  for (; n < 4; n++) out[n] = '\0';

  // Country codes people reach for instead of the language code. Left unmapped they would each
  // open a second bucket for a language that already has one, splitting its totals in half.
  static constexpr struct {
    const char* from;
    const char* to;
  } ALIASES[] = {{"jp", "ja"}, {"cn", "zh"}, {"kr", "ko"}};
  const auto* alias =
      std::find_if(std::begin(ALIASES), std::end(ALIASES), [&out](const auto& a) { return strcmp(out, a.from) == 0; });
  if (alias != std::end(ALIASES)) strncpy(out, alias->to, 4);
}

int daysInMonth(uint16_t y, uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}

bool skipBytes(HalFile& file, size_t count) {
  uint8_t discard[64];
  while (count > 0) {
    const size_t chunk = std::min(count, sizeof(discard));
    const int read = static_cast<int>(file.read(discard, chunk));
    if (read < 0 || static_cast<size_t>(read) != chunk) return false;
    count -= chunk;
  }
  return true;
}
}  // namespace

void ReadingStatsStore::addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes) {
  const int epoch = daysSinceEpoch(year, month, day);
  // From the back: the day added is almost always the newest, so this ends in a step or two.
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e == epoch) {
      // Saturating: a wrap would display a small plausible number instead of an obvious fault.
      const uint32_t sum = static_cast<uint32_t>(it->minutesRead) + minutes;
      it->minutesRead = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
      return;
    }
    if (e < epoch) {
      // Sorted insert. Only a backwards clock jump lands anywhere but the end; the streak
      // passes depend on the ordering.
      days.insert(it.base(), {year, month, day, minutes});
      return;
    }
  }
  days.insert(days.begin(), {year, month, day, minutes});
}

void ReadingStatsStore::addBookMinutes(const char* bookPath, const char* language, const uint16_t minutes,
                                       const uint16_t year, const uint8_t month, const uint8_t day) {
  if (!bookPath || !*bookPath) return;
  const int32_t today = daysSinceEpoch(year, month, day);
  // Clamped to the small-string-optimisation limit: real tags ("ja", "zh-Hant") fit easily, and
  // this keeps each entry's language free of a heap allocation and its on-disk length in a byte.
  // MAX_STORED_LANGUAGE is the same bound the loader enforces on a language read back from disk.
  const std::string lang(language ? language : "", language ? strnlen(language, MAX_STORED_LANGUAGE) : 0);

  for (auto& b : books) {
    if (b.path != bookPath) continue;
    b.minutesRead += minutes;
    b.lastReadDay = today;
    if (!lang.empty()) b.language = lang;
    return;
  }

  if (books.size() >= MAX_BOOKS) {
    auto oldest = std::min_element(books.begin(), books.end(), [](const BookReading& a, const BookReading& b) {
      return a.lastReadDay < b.lastReadDay;
    });
    books.erase(oldest);
  }
  // Deliberately not reserve(MAX_BOOKS): that would commit ~11KB of DRAM the moment the first
  // book is read. This grows one entry per new book, not in a loop.
  books.push_back({bookPath, lang, minutes, today});
}

void ReadingStatsStore::addLanguageMinutes(const char* language, const uint16_t minutes, const uint16_t year,
                                           const uint8_t month, const uint8_t day) {
  char lang[4];
  normalizeLanguage(language, lang);

  const int epoch = daysSinceEpoch(year, month, day);
  LanguageDaily fresh{year, month, day, {}, minutes};
  memcpy(fresh.language, lang, sizeof(lang));

  // Sorted like days, so the per-language streak passes need no scratch buffer.
  for (auto it = languageDays.rbegin(); it != languageDays.rend(); ++it) {
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e == epoch && memcmp(it->language, lang, sizeof(lang)) == 0) {
      const uint32_t sum = static_cast<uint32_t>(it->minutesRead) + minutes;
      it->minutesRead = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
      return;
    }
    if (e <= epoch) {
      // Same date, other language: insert alongside rather than walk the whole run.
      languageDays.insert(it.base(), fresh);
      return;
    }
  }
  languageDays.insert(languageDays.begin(), fresh);
}

uint16_t ReadingStatsStore::getMinutesForDay(const char* language, const uint16_t year, const uint8_t month,
                                             const uint8_t day) const {
  char lang[4];
  normalizeLanguage(language, lang);
  const auto it = std::find_if(languageDays.begin(), languageDays.end(), [&](const LanguageDaily& e) {
    return e.year == year && e.month == month && e.day == day && memcmp(e.language, lang, sizeof(lang)) == 0;
  });
  return it == languageDays.end() ? 0 : it->minutesRead;
}

uint32_t ReadingStatsStore::getTotalMinutes(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  return std::accumulate(languageDays.begin(), languageDays.end(), uint32_t{0},
                         [&lang](const uint32_t sum, const LanguageDaily& e) {
                           return memcmp(e.language, lang, sizeof(lang)) == 0 ? sum + e.minutesRead : sum;
                         });
}

void ReadingStatsStore::getLanguages(std::vector<LanguageSummary>& out) const {
  out.clear();
  for (const auto& e : languageDays) {
    auto it = std::find_if(out.begin(), out.end(),
                           [&e](const LanguageSummary& s) { return memcmp(s.code, e.language, sizeof(s.code)) == 0; });
    if (it != out.end()) {
      it->minutes += e.minutesRead;
      continue;
    }
    LanguageSummary s{};
    memcpy(s.code, e.language, sizeof(s.code));
    s.minutes = e.minutesRead;
    out.push_back(s);
  }
  // Most-read first, so the wanted tab is the one you land on.
  std::sort(out.begin(), out.end(),
            [](const LanguageSummary& a, const LanguageSummary& b) { return a.minutes > b.minutes; });
}

int ReadingStatsStore::getStreak(const char* language, const uint16_t todayYear, const uint8_t todayMonth,
                                 const uint8_t todayDay) const {
  char lang[4];
  normalizeLanguage(language, lang);
  if (getMinutesForDay(language, todayYear, todayMonth, todayDay) == 0) return 0;

  int expected = daysSinceEpoch(todayYear, todayMonth, todayDay);
  int streak = 0;
  for (auto it = languageDays.rbegin(); it != languageDays.rend(); ++it) {
    if (memcmp(it->language, lang, sizeof(lang)) != 0 || it->minutesRead == 0) continue;
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e > expected) continue;
    if (e != expected) break;
    streak++;
    expected--;
  }
  return streak;
}

int ReadingStatsStore::getLongestStreak(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  int maxStreak = 0, cur = 0, prev = 0;
  for (const auto& e : languageDays) {
    if (memcmp(e.language, lang, sizeof(lang)) != 0 || e.minutesRead == 0) continue;
    const int ep = daysSinceEpoch(e.year, e.month, e.day);
    if (cur == 0) {
      cur = 1;
    } else if (ep == prev + 1) {
      cur++;
    } else if (ep != prev) {
      cur = 1;
    }
    prev = ep;
    if (cur > maxStreak) maxStreak = cur;
  }
  return maxStreak;
}

int ReadingStatsStore::getDaysRead(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  return static_cast<int>(std::count_if(languageDays.begin(), languageDays.end(), [&lang](const LanguageDaily& e) {
    return memcmp(e.language, lang, sizeof(lang)) == 0 && e.minutesRead > 0;
  }));
}

uint16_t ReadingStatsStore::getMinutesThisWeek(const char* language, const uint16_t todayYear, const uint8_t todayMonth,
                                               const uint8_t todayDay) const {
  const int dow = (dowFromDate(todayYear, todayMonth, todayDay) + 6) % 7;  // ISO Mon=0
  // Wider accumulator: per-day minutes saturate at UINT16_MAX, so seven of them can exceed it.
  // Wrapping would show a small plausible number rather than an obvious fault.
  uint32_t total = 0;
  for (int i = 0; i <= dow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(language, y, m, d);
  }
  return static_cast<uint16_t>(std::min<uint32_t>(total, UINT16_MAX));
}

void ReadingStatsStore::getWeekStatus(const char* language, const uint16_t todayYear, const uint8_t todayMonth,
                                      const uint8_t todayDay, const int todayDow, bool readDays[7]) const {
  for (int i = 0; i < 7; i++) readDays[i] = false;
  for (int i = 0; i <= todayDow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, todayDow - i);
    readDays[i] = getMinutesForDay(language, y, m, d) > 0;
  }
}

void ReadingStatsStore::getMonthStatus(const char* language, const uint16_t year, const uint8_t month,
                                       bool out[32]) const {
  char lang[4];
  normalizeLanguage(language, lang);
  for (int i = 0; i < 32; i++) out[i] = false;
  for (const auto& e : languageDays) {
    if (memcmp(e.language, lang, sizeof(lang)) != 0 || e.minutesRead == 0) continue;
    if (e.year == year && e.month == month && e.day >= 1 && e.day <= 31) out[e.day] = true;
  }
}

int ReadingStatsStore::getDaysReadInMonth(const char* language, const uint16_t year, const uint8_t month) const {
  bool status[32];
  getMonthStatus(language, year, month, status);
  int count = 0;
  const int dim = daysInMonth(year, month);
  for (int d = 1; d <= dim; d++) {
    if (status[d]) count++;
  }
  return count;
}

uint16_t ReadingStatsStore::getBooksFinished(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  uint16_t count = 0;
  for (const auto& p : finishedBookPaths) {
    const auto it = std::find_if(books.begin(), books.end(), [&p](const BookReading& b) { return b.path == p; });
    if (it == books.end()) continue;  // evicted from the per-book block; language unknown
    char bookLang[4];
    normalizeLanguage(it->language.c_str(), bookLang);
    if (memcmp(bookLang, lang, sizeof(lang)) == 0) count++;
  }
  return count;
}

void ReadingStatsStore::markBookFinished(const std::string& bookPath) { setBookFinished(bookPath, true); }

bool ReadingStatsStore::isBookFinished(const std::string& bookPath) const {
  return std::find(finishedBookPaths.begin(), finishedBookPaths.end(), bookPath) != finishedBookPaths.end();
}

bool ReadingStatsStore::setBookFinished(const std::string& bookPath, const bool finished) {
  if (bookPath.empty() || bookPath.size() > MAX_PERSISTED_PATH_BYTES) return false;
  const auto it = std::find(finishedBookPaths.begin(), finishedBookPaths.end(), bookPath);
  if (finished) {
    if (it != finishedBookPaths.end() || finishedBookPaths.size() >= MAX_FINISHED_BOOKS) return false;
    finishedBookPaths.push_back(bookPath);
    finishedBookRatings.push_back(0);
  } else {
    if (it == finishedBookPaths.end()) return false;
    const size_t index = static_cast<size_t>(it - finishedBookPaths.begin());
    finishedBookPaths.erase(it);
    if (index < finishedBookRatings.size()) finishedBookRatings.erase(finishedBookRatings.begin() + index);
  }
  // Once the path block is intact (the normal case), this is the current Completed collection.
  // loadFromFile() already derives the same count from this list. Manual "unfinished" must be
  // allowed to count down rather than preserving a lifetime high-water mark.
  booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
  return true;
}

uint8_t ReadingStatsStore::getBookRating(const std::string& bookPath) const {
  const auto it = std::find(finishedBookPaths.begin(), finishedBookPaths.end(), bookPath);
  if (it == finishedBookPaths.end()) return 0;
  const size_t index = static_cast<size_t>(it - finishedBookPaths.begin());
  return index < finishedBookRatings.size() ? finishedBookRatings[index] : 0;
}

bool ReadingStatsStore::setBookRating(const std::string& bookPath, const uint8_t rating) {
  if (rating > 5) return false;
  const auto it = std::find(finishedBookPaths.begin(), finishedBookPaths.end(), bookPath);
  if (it == finishedBookPaths.end()) return false;
  const size_t index = static_cast<size_t>(it - finishedBookPaths.begin());
  if (finishedBookRatings.size() < finishedBookPaths.size()) finishedBookRatings.resize(finishedBookPaths.size(), 0);
  if (finishedBookRatings[index] == rating) return false;
  finishedBookRatings[index] = rating;
  return true;
}

bool ReadingStatsStore::updateBookPath(const std::string& oldPath, const std::string& newPath) {
  if (oldPath.empty() || newPath.empty() || oldPath == newPath) return false;
  bool changed = false;

  const auto oldFinished = std::find(finishedBookPaths.begin(), finishedBookPaths.end(), oldPath);
  if (oldFinished != finishedBookPaths.end()) {
    const size_t oldIndex = static_cast<size_t>(oldFinished - finishedBookPaths.begin());
    const auto newFinished = std::find(finishedBookPaths.begin(), finishedBookPaths.end(), newPath);
    if (newFinished == finishedBookPaths.end()) {
      *oldFinished = newPath;
    } else {
      const size_t newIndex = static_cast<size_t>(newFinished - finishedBookPaths.begin());
      if (oldIndex < finishedBookRatings.size() && newIndex < finishedBookRatings.size() &&
          finishedBookRatings[newIndex] == 0) {
        finishedBookRatings[newIndex] = finishedBookRatings[oldIndex];
      }
      finishedBookPaths.erase(oldFinished);
      if (oldIndex < finishedBookRatings.size()) finishedBookRatings.erase(finishedBookRatings.begin() + oldIndex);
    }
    changed = true;
  }

  for (auto& book : books) {
    if (book.path != oldPath) continue;
    book.path = newPath;
    changed = true;
  }
  if (changed) booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
  return changed;
}

uint16_t ReadingStatsStore::getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const {
  // Back to front: every caller here asks about recent days (today, this week, the month on
  // screen), which now sit at the end of a potentially years-long history.
  const auto it = std::find_if(days.rbegin(), days.rend(), [year, month, day](const DailyReading& d) {
    return d.year == year && d.month == month && d.day == day;
  });
  return it == days.rend() ? 0 : it->minutesRead;
}

bool ReadingStatsStore::hasReadToday(uint16_t year, uint8_t month, uint8_t day) const {
  return getMinutesForDay(year, month, day) > 0;
}

bool ReadingStatsStore::readFinishedCountFromFile(uint16_t& outCount) {
  outCount = 0;
  HalFile f;
  if (!Storage.openFileForRead("STAT", statsPath().c_str(), f)) return false;

  uint8_t version = 0;
  uint16_t dayCount = 0;
  const bool prefixRead =
      f.read(&version, 1) == 1 && f.read(reinterpret_cast<uint8_t*>(&dayCount), sizeof(dayCount)) == sizeof(dayCount);
  const bool countRead =
      prefixRead && version >= 2 && f.read(reinterpret_cast<uint8_t*>(&outCount), sizeof(outCount)) == sizeof(outCount);
  f.close();
  if (!countRead) outCount = 0;
  return countRead;
}

uint64_t ReadingStatsStore::finishedPathHash(const std::string_view path) {
  return library::clixPathHash(path.data(), path.size());
}

bool ReadingStatsStore::readFinishedPathHashesFromFile(std::unique_ptr<uint64_t[]>& outHashes, uint16_t& outCount) {
  outHashes.reset();
  outCount = 0;
  const std::string path = statsPath();
  if (!Storage.exists(path.c_str())) return true;

  HalFile f;
  if (!Storage.openFileForRead("STAT", path.c_str(), f)) return false;
  const auto fail = [&]() {
    f.close();
    outHashes.reset();
    outCount = 0;
    return false;
  };

  uint8_t version = 0;
  uint16_t dayCount = 0;
  uint16_t headerFinishedCount = 0;
  if (f.read(&version, 1) != 1 || f.read(reinterpret_cast<uint8_t*>(&dayCount), 2) != 2 || version < 2 ||
      f.read(reinterpret_cast<uint8_t*>(&headerFinishedCount), 2) != 2 ||
      !skipBytes(f, static_cast<size_t>(dayCount) * sizeof(DailyReading))) {
    return fail();
  }

  uint16_t pathCount = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&pathCount), 2) != 2 || pathCount > MAX_FINISHED_BOOKS) {
    return fail();
  }
  // The path block is authoritative. Older builds could preserve a lifetime/high-water count in
  // the header, so rejecting a valid path block on that mismatch hid every shelf even though the
  // completed preview could still read the same file. A mutation load normalizes the header on
  // its next safe save.
  (void)headerFinishedCount;
  auto hashes = makeUniqueNoThrow<uint64_t[]>(pathCount == 0 ? 1 : pathCount);
  if (!hashes) return fail();

  uint8_t chunk[64];
  for (uint16_t index = 0; index < pathCount; index++) {
    uint16_t pathLength = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&pathLength), 2) != 2 || pathLength > MAX_PERSISTED_PATH_BYTES) return fail();
    uint64_t hash = 14695981039346656037ULL;
    size_t remaining = pathLength;
    while (remaining > 0) {
      const size_t bytes = std::min(remaining, sizeof(chunk));
      if (f.read(chunk, bytes) != bytes) return fail();
      for (size_t offset = 0; offset < bytes; offset++) {
        hash = library::clixPathHashByte(hash, chunk[offset]);
      }
      remaining -= bytes;
    }
    hashes[index] = hash;
  }
  f.close();
  std::sort(hashes.get(), hashes.get() + pathCount);
  outHashes = std::move(hashes);
  outCount = pathCount;
  return true;
}

bool ReadingStatsStore::readFinishedPreviewFromFile(std::vector<FinishedBookPreview>& out, const size_t maxBooks,
                                                    uint16_t& outTotal, uint16_t& outRatedCount, uint32_t& outRatingSum,
                                                    const std::vector<std::string>* membershipCandidates,
                                                    std::vector<uint8_t>* outMembership, const size_t newestOffset,
                                                    const bool existingOnly, const uint64_t* existingPathHashes,
                                                    const size_t existingPathHashCount) {
  out.clear();
  outTotal = 0;
  outRatedCount = 0;
  outRatingSum = 0;
  out.reserve(std::min<size_t>(maxBooks, MAX_FINISHED_BOOKS));
  if (outMembership) {
    outMembership->assign(membershipCandidates ? membershipCandidates->size() : 0, 0);
  }

  HalFile f;
  if (!Storage.openFileForRead("STAT", statsPath().c_str(), f)) return false;

  uint8_t version = 0;
  uint16_t dayCount = 0;
  if (f.read(&version, 1) != 1 || f.read(reinterpret_cast<uint8_t*>(&dayCount), 2) != 2 || version < 2 ||
      f.read(reinterpret_cast<uint8_t*>(&outTotal), 2) != 2) {
    return false;
  }

  if (!skipBytes(f, static_cast<size_t>(dayCount) * sizeof(DailyReading))) return false;

  uint16_t pathCount = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&pathCount), 2) != 2 || pathCount > MAX_FINISHED_BOOKS) return false;
  // Paths may be 500 bytes, above the firmware's per-function stack budget. One bounded nothrow
  // allocation per Hub load is safer than a 501-byte stack frame and is reused for every record.
  auto pathBuffer = makeUniqueNoThrow<char[]>(MAX_PERSISTED_PATH_BYTES + 1);
  if (!pathBuffer) return false;

  std::unique_ptr<uint8_t[]> existingBits;
  size_t existingCount = pathCount;
  if (existingOnly && pathCount > 0) {
    const size_t bitBytes = (pathCount + 7u) / 8u;
    existingBits = makeUniqueNoThrow<uint8_t[]>(bitBytes);
    if (!existingBits) return false;
    memset(existingBits.get(), 0, bitBytes);
    existingCount = 0;
    const bool inspectMembership = membershipCandidates && outMembership && !membershipCandidates->empty();
    for (uint16_t index = 0; index < pathCount; index++) {
      uint16_t pathLength = 0;
      if (f.read(reinterpret_cast<uint8_t*>(&pathLength), 2) != 2 || pathLength > MAX_PERSISTED_PATH_BYTES ||
          (pathLength > 0 && f.read(reinterpret_cast<uint8_t*>(pathBuffer.get()), pathLength) != pathLength)) {
        return false;
      }
      pathBuffer[pathLength] = '\0';
      if (inspectMembership) {
        for (size_t candidate = 0; candidate < membershipCandidates->size(); candidate++) {
          const auto& value = (*membershipCandidates)[candidate];
          if (value.size() == pathLength && memcmp(value.data(), pathBuffer.get(), pathLength) == 0) {
            (*outMembership)[candidate] = 1;
          }
        }
      }
      const bool exists = existingPathHashes
                              ? std::binary_search(existingPathHashes, existingPathHashes + existingPathHashCount,
                                                   finishedPathHash(std::string_view{pathBuffer.get(), pathLength}))
                              : Storage.exists(pathBuffer.get());
      if (exists) {
        existingBits[index / 8u] |= static_cast<uint8_t>(1u << (index % 8u));
        existingCount++;
      }
    }
    f.close();
    if (!Storage.openFileForRead("STAT", statsPath().c_str(), f)) return false;
    uint8_t secondVersion = 0;
    uint16_t secondDayCount = 0;
    uint16_t secondStoredTotal = 0;
    uint16_t secondPathCount = 0;
    if (f.read(&secondVersion, 1) != 1 || f.read(reinterpret_cast<uint8_t*>(&secondDayCount), 2) != 2 ||
        secondVersion != version || secondDayCount != dayCount ||
        f.read(reinterpret_cast<uint8_t*>(&secondStoredTotal), 2) != 2 || secondStoredTotal != outTotal ||
        !skipBytes(f, static_cast<size_t>(secondDayCount) * sizeof(DailyReading)) ||
        f.read(reinterpret_cast<uint8_t*>(&secondPathCount), 2) != 2 || secondPathCount != pathCount) {
      return false;
    }
  }

  const size_t retainedEnd = existingCount > newestOffset ? existingCount - newestOffset : 0;
  const size_t retainedStart = retainedEnd > maxBooks ? retainedEnd - maxBooks : 0;
  size_t existingOrdinal = 0;
  for (uint16_t index = 0; index < pathCount; index++) {
    uint16_t pathLength = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&pathLength), 2) != 2 || pathLength > MAX_PERSISTED_PATH_BYTES) return false;
    const bool exists = !existingOnly || (existingBits[index / 8u] & (1u << (index % 8u))) != 0;
    const size_t candidateOrdinal = existingOnly ? existingOrdinal : index;
    const bool keep = exists && candidateOrdinal >= retainedStart && candidateOrdinal < retainedEnd;
    const bool inspectMembership =
        !existingOnly && membershipCandidates && outMembership && !membershipCandidates->empty();
    if (keep || inspectMembership) {
      if (pathLength > 0 && f.read(reinterpret_cast<uint8_t*>(pathBuffer.get()), pathLength) != pathLength) {
        return false;
      }
      pathBuffer[pathLength] = '\0';
      if (inspectMembership) {
        for (size_t candidate = 0; candidate < membershipCandidates->size(); candidate++) {
          const auto& value = (*membershipCandidates)[candidate];
          if (value.size() == pathLength && memcmp(value.data(), pathBuffer.get(), pathLength) == 0) {
            (*outMembership)[candidate] = 1;
          }
        }
      }
    }
    if (keep) {
      FinishedBookPreview preview;
      preview.path.assign(pathBuffer.get(), pathLength);
      out.push_back(std::move(preview));
    } else if (!inspectMembership && !skipBytes(f, pathLength)) {
      return false;
    }
    if (exists) existingOrdinal++;
  }

  // The path block is the authoritative completion record. Optional blocks written after it can
  // be interrupted by power loss; preserve the valid paths as unrated instead of making every
  // completion disappear from Home. This mirrors loadFromFile()'s fail-soft policy.
  const auto finishWithoutRatings = [&]() {
    outRatedCount = 0;
    outRatingSum = 0;
    for (auto& preview : out) preview.rating = 0;
    std::reverse(out.begin(), out.end());
    outTotal = static_cast<uint16_t>(existingCount);
    return true;
  };

  if (version >= 3) {
    uint16_t bookCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&bookCount), 2) != 2 || bookCount > MAX_BOOKS) {
      return finishWithoutRatings();
    }
    for (uint16_t index = 0; index < bookCount; index++) {
      uint16_t pathLength = 0;
      uint8_t languageLength = 0;
      if (f.read(reinterpret_cast<uint8_t*>(&pathLength), 2) != 2 || pathLength > MAX_PERSISTED_PATH_BYTES ||
          !skipBytes(f, pathLength) || f.read(&languageLength, 1) != 1 || languageLength > MAX_STORED_LANGUAGE ||
          !skipBytes(f, static_cast<size_t>(languageLength) + 8)) {
        return finishWithoutRatings();
      }
    }
  }

  if (version >= 4) {
    uint16_t languageDayCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&languageDayCount), 2) != 2 ||
        !skipBytes(f, static_cast<size_t>(languageDayCount) * 10)) {
      return finishWithoutRatings();
    }
  }

  if (version >= 5) {
    uint16_t ratingCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&ratingCount), 2) != 2 || ratingCount != pathCount) {
      return finishWithoutRatings();
    }
    existingOrdinal = 0;
    for (uint16_t index = 0; index < ratingCount; index++) {
      uint8_t rating = 0;
      if (f.read(&rating, 1) != 1 || rating > 5) return finishWithoutRatings();
      const bool exists = !existingOnly || (existingBits[index / 8u] & (1u << (index % 8u))) != 0;
      if (exists && rating > 0) {
        outRatedCount++;
        outRatingSum += rating;
      }
      if (!exists) continue;
      const size_t ratingOrdinal = existingOnly ? existingOrdinal : index;
      if (ratingOrdinal >= retainedStart && ratingOrdinal < retainedEnd) {
        out[ratingOrdinal - retainedStart].rating = rating;
      }
      if (existingOnly) existingOrdinal++;
    }
  }

  std::reverse(out.begin(), out.end());
  outTotal = static_cast<uint16_t>(existingCount);
  return true;
}

int ReadingStatsStore::getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  // Backwards through the sorted history, rather than probing one candidate date per day.
  if (days.empty()) return 0;
  int expected = daysSinceEpoch(todayYear, todayMonth, todayDay);
  if (getMinutesForDay(todayYear, todayMonth, todayDay) == 0) return 0;

  int streak = 0;
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if (it->minutesRead == 0) continue;
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e > expected) continue;  // days after today (a clock jump) do not extend today's streak
    if (e != expected) break;    // a gap ends it
    streak++;
    expected--;
  }
  return streak;
}

int ReadingStatsStore::getLongestStreak() const {
  // Single pass over sorted history: no scratch buffer (the old int[365] lived on the STACK,
  // which an unbounded history would overflow) and no O(n^2) sort.
  int maxStreak = 0, cur = 0, prev = 0;
  for (const auto& d : days) {
    if (d.minutesRead == 0) continue;
    const int e = daysSinceEpoch(d.year, d.month, d.day);
    if (cur == 0) {
      cur = 1;
    } else if (e == prev + 1) {
      cur++;
    } else if (e != prev) {
      cur = 1;
    }
    prev = e;
    if (cur > maxStreak) maxStreak = cur;
  }
  return maxStreak;
}

int ReadingStatsStore::getDaysRead() const { return static_cast<int>(days.size()); }

uint32_t ReadingStatsStore::getTotalMinutes() const {
  return std::accumulate(days.begin(), days.end(), uint32_t{0},
                         [](const uint32_t sum, const DailyReading& d) { return sum + d.minutesRead; });
}

uint16_t ReadingStatsStore::getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  int dow = (dowFromDate(todayYear, todayMonth, todayDay) + 6) % 7;  // ISO Mon=0
  uint32_t total = 0;  // see the per-language overload: seven saturated days overflow a uint16
  for (int i = 0; i <= dow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(y, m, d);
  }
  return static_cast<uint16_t>(std::min<uint32_t>(total, UINT16_MAX));
}

void ReadingStatsStore::getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay, int todayDow,
                                      bool readDays[7]) const {
  for (int i = 0; i < 7; i++) readDays[i] = false;
  for (int i = 0; i <= todayDow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, todayDow - i);
    readDays[i] = getMinutesForDay(y, m, d) > 0;
  }
}

void ReadingStatsStore::getMonthStatus(uint16_t year, uint8_t month, bool out[32]) const {
  for (int i = 0; i < 32; i++) out[i] = false;
  int dim = daysInMonth(year, month);
  for (int d = 1; d <= dim; d++) {
    out[d] = getMinutesForDay(year, month, static_cast<uint8_t>(d)) > 0;
  }
}

int ReadingStatsStore::getDaysReadInMonth(uint16_t year, uint8_t month) const {
  int count = 0;
  int dim = daysInMonth(year, month);
  for (int d = 1; d <= dim; d++) {
    if (getMinutesForDay(year, month, static_cast<uint8_t>(d)) > 0) count++;
  }
  return count;
}

bool ReadingStatsStore::saveToFile() const {
  HalFile f;
  if (!Storage.openFileForWrite("STAT", statsPath().c_str(), f)) return false;
  f.write(&STATS_VERSION, 1);
  // days never exceeds MAX_DAYS in memory, so this only guards the 16-bit field.
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(days.size(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&count), 2);
  f.write(reinterpret_cast<const uint8_t*>(&booksFinished), 2);
  for (uint16_t i = 0; i < count; i++) {
    f.write(reinterpret_cast<const uint8_t*>(&days[i]), sizeof(DailyReading));
  }
  // Write finished book paths
  uint16_t pathCount = static_cast<uint16_t>(finishedBookPaths.size());
  f.write(reinterpret_cast<const uint8_t*>(&pathCount), 2);
  for (const auto& p : finishedBookPaths) {
    uint16_t len = static_cast<uint16_t>(p.size());
    f.write(reinterpret_cast<const uint8_t*>(&len), 2);
    f.write(reinterpret_cast<const uint8_t*>(p.data()), len);
  }
  // Per-book block (v3+)
  uint16_t bookCount = static_cast<uint16_t>(books.size());
  f.write(reinterpret_cast<const uint8_t*>(&bookCount), 2);
  for (const auto& b : books) {
    uint16_t pathLen = static_cast<uint16_t>(b.path.size());
    f.write(reinterpret_cast<const uint8_t*>(&pathLen), 2);
    f.write(reinterpret_cast<const uint8_t*>(b.path.data()), pathLen);
    uint8_t langLen = static_cast<uint8_t>(b.language.size());
    f.write(&langLen, 1);
    f.write(reinterpret_cast<const uint8_t*>(b.language.data()), langLen);
    f.write(reinterpret_cast<const uint8_t*>(&b.minutesRead), 4);
    f.write(reinterpret_cast<const uint8_t*>(&b.lastReadDay), 4);
  }
  // Per-day-per-language block (v4+). Fields written individually rather than as a struct blob:
  // LanguageDaily has trailing padding on some targets, and a padded layout would not survive a
  // format change on the reading side.
  // 16-bit count, as the format defines; clamped rather than silently truncated lower.
  const uint16_t langDayCount = static_cast<uint16_t>(std::min<size_t>(languageDays.size(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&langDayCount), 2);
  size_t written = 0;
  for (const auto& e : languageDays) {
    if (written++ >= langDayCount) break;
    f.write(reinterpret_cast<const uint8_t*>(&e.year), 2);
    f.write(&e.month, 1);
    f.write(&e.day, 1);
    f.write(reinterpret_cast<const uint8_t*>(e.language), 4);
    f.write(reinterpret_cast<const uint8_t*>(&e.minutesRead), 2);
  }
  // Ratings block (v5+), aligned one-for-one with the finished path block. Write a zero for any
  // missing in-memory slot so a recovered legacy/corrupt file is normalized on its next save.
  f.write(reinterpret_cast<const uint8_t*>(&pathCount), 2);
  for (size_t i = 0; i < pathCount; i++) {
    const uint8_t rating = i < finishedBookRatings.size() && finishedBookRatings[i] <= 5 ? finishedBookRatings[i]
                                                                                         : static_cast<uint8_t>(0);
    f.write(&rating, 1);
  }
  f.close();
  return true;
}

bool ReadingStatsStore::loadFromFile() { return loadFromFileImpl(false); }

bool ReadingStatsStore::loadFromFileForMutation() {
  const std::string path = statsPath();
  return !Storage.exists(path.c_str()) || loadFromFileImpl(true);
}

bool ReadingStatsStore::loadFromFileImpl(const bool requireComplete) {
  HalFile f;
  if (!Storage.openFileForRead("STAT", statsPath().c_str(), f)) return false;
  uint8_t version;
  if (f.read(&version, 1) != 1) {
    f.close();
    return false;
  }
  uint16_t count;
  if (f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2) {
    f.close();
    return false;
  }
  if (version >= 2) {
    if (f.read(reinterpret_cast<uint8_t*>(&booksFinished), 2) != 2) {
      f.close();
      return false;
    }
  }
  // Keep the most recent MAX_DAYS and CONSUME the rest: skipping them by clamping `count`
  // would leave the file positioned mid-block and misalign everything after it.
  days.clear();
  bool daysIntact = true;
  const size_t keepDays = std::min<size_t>(count, MAX_DAYS);
  for (size_t i = 0; i + keepDays < count; i++) {
    DailyReading discard;
    if (f.read(reinterpret_cast<uint8_t*>(&discard), sizeof(DailyReading)) != sizeof(DailyReading)) {
      daysIntact = false;
      break;
    }
  }
  days.reserve(keepDays);
  for (size_t i = 0; daysIntact && i < keepDays; i++) {
    DailyReading dr;
    if (f.read(reinterpret_cast<uint8_t*>(&dr), sizeof(DailyReading)) != sizeof(DailyReading)) {
      daysIntact = false;
      break;
    }
    // Drop entries recorded while the system clock was unset (RTC-less devices booted at the
    // 1970 epoch before HalClock::restoreSystemTime existed) -- they are misdated garbage that
    // pollutes streaks, totals, and the calendar.
    if (dr.year < 2020) continue;
    days.push_back(dr);
  }
  // The streak passes assume ascending order. Normally one comparison pass, since files are
  // written in order; a file that is not must not silently yield wrong streaks.
  if (!std::is_sorted(days.begin(), days.end(), [](const DailyReading& a, const DailyReading& b) {
        return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
      })) {
    std::sort(days.begin(), days.end(), [](const DailyReading& a, const DailyReading& b) {
      return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
    });
  }
  // Read finished book paths
  finishedBookPaths.clear();
  finishedBookRatings.clear();
  bool pathsIntact = false;
  uint16_t pathCount = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&pathCount), 2) == 2 && pathCount <= 500) {
    pathsIntact = true;
    for (int i = 0; i < pathCount; i++) {
      uint16_t len = 0;
      if (f.read(reinterpret_cast<uint8_t*>(&len), 2) != 2 || len > 500) {
        pathsIntact = false;
        break;
      }
      std::string p(len, '\0');
      if (f.read(reinterpret_cast<uint8_t*>(&p[0]), len) != len) {
        pathsIntact = false;
        break;
      }
      finishedBookPaths.push_back(std::move(p));
    }
    // Only trust the list's length once it read whole. On a truncated file the header's count is
    // the better answer: leaving a non-zero booksFinished beside a partial list would let
    // markBookFinished() overwrite the real total with the partial one on the next save.
    if (pathsIntact) booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
  }
  // Per-book block (v3+). Only readable when the paths block above was consumed whole -- a short
  // read there leaves the file position mid-record, so anything after it is misaligned garbage.
  books.clear();
  bool booksIntact = version < 3;
  if (version >= 3 && pathsIntact) {
    uint16_t bookCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&bookCount), 2) == 2 && bookCount <= MAX_BOOKS) {
      books.reserve(bookCount);
      for (int i = 0; i < bookCount; i++) {
        BookReading b;
        uint16_t pathLen = 0;
        if (f.read(reinterpret_cast<uint8_t*>(&pathLen), 2) != 2 || pathLen > 500) break;
        b.path.assign(pathLen, '\0');
        if (pathLen && f.read(reinterpret_cast<uint8_t*>(&b.path[0]), pathLen) != pathLen) break;
        uint8_t langLen = 0;
        if (f.read(&langLen, 1) != 1 || langLen > MAX_STORED_LANGUAGE) break;
        b.language.assign(langLen, '\0');
        if (langLen && f.read(reinterpret_cast<uint8_t*>(&b.language[0]), langLen) != langLen) break;
        if (f.read(reinterpret_cast<uint8_t*>(&b.minutesRead), 4) != 4) break;
        if (f.read(reinterpret_cast<uint8_t*>(&b.lastReadDay), 4) != 4) break;
        books.push_back(std::move(b));
      }
      // Only a complete block leaves the file positioned at the start of the next one; any
      // short read above stopped mid-record.
      booksIntact = books.size() == bookCount;
    }
  }
  // Per-day-per-language block (v4+)
  languageDays.clear();
  bool languageDaysIntact = version < 4;
  if (version >= 4 && booksIntact) {
    uint16_t langDayCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&langDayCount), 2) == 2) {
      // Same bounded-tail read as the day block above: a uint16 count is 640KB of records at
      // worst, and reserve() aborts under -fno-exceptions rather than failing.
      const size_t keepLang = std::min<size_t>(langDayCount, MAX_LANG_DAYS);
      bool readIntact = true;
      for (size_t i = 0; i + keepLang < langDayCount; i++) {
        uint8_t discard[10];
        if (f.read(discard, sizeof(discard)) != sizeof(discard)) {
          readIntact = false;
          break;
        }
      }
      languageDays.reserve(keepLang);
      for (size_t i = 0; readIntact && i < keepLang; i++) {
        LanguageDaily e{};
        if (f.read(reinterpret_cast<uint8_t*>(&e.year), 2) != 2 || f.read(&e.month, 1) != 1 || f.read(&e.day, 1) != 1 ||
            f.read(reinterpret_cast<uint8_t*>(e.language), 4) != 4 ||
            f.read(reinterpret_cast<uint8_t*>(&e.minutesRead), 2) != 2) {
          readIntact = false;
          break;
        }
        e.language[3] = '\0';         // a corrupt record must not leave an unterminated tag
        if (e.year < 2020) continue;  // same unset-clock garbage the per-day loop drops
        languageDays.push_back(e);
      }
      languageDaysIntact = readIntact;
      // Same ordering guarantee as days, for the same passes.
      const auto byDate = [](const LanguageDaily& a, const LanguageDaily& b) {
        return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
      };
      if (!std::is_sorted(languageDays.begin(), languageDays.end(), byDate)) {
        std::stable_sort(languageDays.begin(), languageDays.end(), byDate);
      }
    }
  }
  // Ratings (v5+). Any older, truncated, mismatched, or out-of-range block safely becomes
  // "unrated" without losing the completed paths themselves.
  finishedBookRatings.assign(finishedBookPaths.size(), 0);
  bool ratingsIntact = version < 5;
  if (version >= 5 && pathsIntact && booksIntact && languageDaysIntact) {
    uint16_t ratingCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&ratingCount), 2) == 2 && ratingCount == finishedBookPaths.size()) {
      ratingsIntact = true;
      for (size_t i = 0; i < ratingCount; i++) {
        uint8_t rating = 0;
        if (f.read(&rating, 1) != 1 || rating > 5) {
          ratingsIntact = false;
          break;
        }
        finishedBookRatings[i] = rating;
      }
      if (!ratingsIntact) std::fill(finishedBookRatings.begin(), finishedBookRatings.end(), 0);
    }
  }
  f.close();
  return !requireComplete || (version >= 1 && version <= STATS_VERSION && daysIntact && pathsIntact && booksIntact &&
                              languageDaysIntact && ratingsIntact);
}
