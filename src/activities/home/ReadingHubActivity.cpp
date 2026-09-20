#include "ReadingHubActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>

#include <algorithm>
#include <cstdio>

#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"

ReadingHubActivity::ReadingHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const HomeMenuItem initialMenuItemValue, const bool cleanInitialRefreshValue)
    : Activity("Home", renderer, mappedInput),
      initialMenuItem(initialMenuItemValue),
      cleanInitialRefresh(cleanInitialRefreshValue) {}

void ReadingHubActivity::onEnter() {
  Activity::onEnter();

  switch (initialMenuItem) {
    case HomeMenuItem::LIBRARY:
    case HomeMenuItem::FILE_BROWSER:
      section = Section::LIBRARY;
      break;
    case HomeMenuItem::READING_STATS:
    case HomeMenuItem::COMPLETED_LIBRARY:
      section = Section::READ;
      break;
    case HomeMenuItem::READING_QUEUE:
      section = Section::QUEUE;
      break;
    default:
      section = Section::NOW;
      break;
  }

  currentBook = nullptr;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!RecentBooksStore::isMissing(book)) {
      currentBook = &book;
      break;
    }
  }

  queueStore.clear();
  queueStore.loadFromFile();
  library::LibraryIndexFile index;
  libraryCountKnown = index.open(library::libraryIndexPath());
  libraryBookCount = libraryCountKnown ? index.bookCount() : 0;
  index.close();
  ReadingStatsStore::readFinishedCountFromFile(completedBookCount);
  selectedRow = 0;
  menuOpen = false;
  firstPaint = true;
  requestUpdate();
}

void ReadingHubActivity::onExit() {
  currentBook = nullptr;
  queueStore.clear();
  Activity::onExit();
}

const char* ReadingHubActivity::sectionLabel() const {
  static constexpr StrId SECTION_LABELS[SECTION_COUNT] = {StrId::STR_HUB_NOW, StrId::STR_HUB_LIBRARY,
                                                          StrId::STR_HUB_QUEUE, StrId::STR_HUB_READ};
  return I18N.get(SECTION_LABELS[static_cast<int>(section)]);
}

int ReadingHubActivity::buildRows(std::array<ReadingHubRow, MAX_ROWS>& rows, RowText& text) const {
  if (menuOpen) {
    rows[0] = {tr(STR_BROWSE_FILES), ""};
    rows[1] = {tr(STR_FILE_TRANSFER), ""};
    rows[2] = {tr(STR_STATS), ""};
    rows[3] = {tr(STR_SETTINGS_TITLE), ""};
    return 4;
  }

  const unsigned queueSize = static_cast<unsigned>(queueStore.queue().size());
  if (queueSize == 0) {
    snprintf(text.queueStatus, sizeof(text.queueStatus), "%s", tr(STR_HUB_QUEUE_EMPTY));
  } else {
    snprintf(text.queueStatus, sizeof(text.queueStatus), tr(STR_HUB_QUEUE_COUNT), queueSize);
  }
  if (libraryCountKnown) {
    snprintf(text.libraryStatus, sizeof(text.libraryStatus), tr(STR_HUB_LIBRARY_COUNT),
             static_cast<unsigned>(libraryBookCount));
  } else {
    snprintf(text.libraryStatus, sizeof(text.libraryStatus), "%s", tr(STR_HUB_ALL_BOOKS));
  }
  snprintf(text.completedStatus, sizeof(text.completedStatus), tr(STR_HUB_COMPLETED_COUNT),
           static_cast<unsigned>(completedBookCount));

  switch (section) {
    case Section::NOW:
      rows[0] = {currentBook ? currentBook->title.c_str() : tr(STR_NO_OPEN_BOOK),
                 currentBook ? currentBook->author.c_str() : tr(STR_START_READING)};
      rows[1] = {tr(STR_HUB_OPEN_QUEUE), text.queueStatus};
      rows[2] = {tr(STR_HUB_COMPLETED_BOOKS), text.completedStatus};
      return 3;
    case Section::LIBRARY:
      rows[0] = {tr(STR_HUB_OPEN_LIBRARY), text.libraryStatus};
      rows[1] = {tr(STR_BROWSE_FILES), ""};
      return 2;
    case Section::QUEUE:
      rows[0] = {tr(STR_HUB_OPEN_QUEUE), text.queueStatus};
      return 1;
    case Section::READ:
      rows[0] = {tr(STR_HUB_COMPLETED_BOOKS), text.completedStatus};
      rows[1] = {tr(STR_STATS), ""};
      return 2;
  }
  return 0;
}

void ReadingHubActivity::stepSection(const int delta) {
  if (menuOpen) return;
  section = reading_hub::stepSection(section, delta);
  selectedRow = 0;
  requestUpdate();
}

void ReadingHubActivity::stepRow(const int delta) {
  std::array<ReadingHubRow, MAX_ROWS> rows{};
  RowText text;
  const int rowCount = buildRows(rows, text);
  if (rowCount <= 0) return;
  selectedRow = reading_hub::stepRow(selectedRow, delta, rowCount);
  requestUpdate();
}

void ReadingHubActivity::activateSelection() {
  if (menuOpen) {
    switch (selectedRow) {
      case 0:
        activityManager.goToFileBrowser();
        return;
      case 1:
        activityManager.goToFileTransfer();
        return;
      case 2:
        activityManager.goToReadingStats();
        return;
      case 3:
        activityManager.goToSettings();
        return;
      default:
        return;
    }
  }

  switch (section) {
    case Section::NOW:
      if (selectedRow == 0 && currentBook) {
        activityManager.goToReader(currentBook->path, true);
      } else if (selectedRow == 0) {
        activityManager.goToLibrary();
      } else if (selectedRow == 1) {
        activityManager.goToReadingQueue();
      } else {
        activityManager.goToCompletedLibrary();
      }
      return;
    case Section::LIBRARY:
      if (selectedRow == 0) {
        activityManager.goToLibrary();
      } else {
        activityManager.goToFileBrowser();
      }
      return;
    case Section::QUEUE:
      activityManager.goToReadingQueue();
      return;
    case Section::READ:
      if (selectedRow == 0) {
        activityManager.goToCompletedLibrary();
      } else {
        activityManager.goToReadingStats();
      }
      return;
  }
}

void ReadingHubActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    menuOpen = !menuOpen;
    selectedRow = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    stepSection(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    stepSection(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    stepRow(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    stepRow(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelection();
}

void ReadingHubActivity::render(RenderLock&&) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  std::array<ReadingHubRow, MAX_ROWS> rows{};
  RowText text;
  const int rowCount = buildRows(rows, text);
  selectedRow = std::clamp(selectedRow, 0, std::max(0, rowCount - 1));

  char title[40] = {0};
  if (menuOpen) {
    snprintf(title, sizeof(title), "%s", tr(STR_HUB_MENU));
  } else {
    snprintf(title, sizeof(title), tr(STR_HUB_SECTION_POSITION), sectionLabel(), static_cast<unsigned>(section) + 1);
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, title);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect content{metrics.contentSidePadding, contentTop, width - metrics.contentSidePadding * 2,
                     std::max(0, contentBottom - contentTop)};
  GUI.drawReadingHubRows(renderer, content, rows.data(), rowCount, selectedRow);

  const auto labels = mappedInput.mapLabels(menuOpen ? tr(STR_BACK) : tr(STR_HUB_MENU), tr(STR_OPEN), tr(STR_HUB_MOVE),
                                            tr(STR_HUB_MOVE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_HUB_PREVIOUS_SECTION), tr(STR_HUB_NEXT_SECTION));

  renderer.displayBuffer(cleanInitialRefresh && firstPaint ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstPaint = false;
}
