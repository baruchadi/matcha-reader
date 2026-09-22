#include "ReadingHubTheme.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "RecentBook.h"
#include "SeriesMetadata.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "fontIds.h"

namespace {
constexpr int CARD_RADIUS = 4;
constexpr int GRID_COLUMNS = 3;
constexpr int GRID_GAP = 10;
constexpr int COVER_ASPECT_NUM = 2;
constexpr int COVER_ASPECT_DEN = 3;

void drawChevron(const GfxRenderer& renderer, const int x, const int y, const bool black) {
  renderer.drawLine(x - 5, y - 6, x + 1, y, black);
  renderer.drawLine(x + 1, y, x - 5, y + 6, black);
}

void drawMiniStar(const GfxRenderer& renderer, const int centerX, const int centerY, const bool filled,
                  const bool black) {
  static constexpr int REL_X[10] = {0, 2, 6, 3, 4, 0, -4, -3, -6, -2};
  static constexpr int REL_Y[10] = {-6, -2, -2, 1, 5, 3, 5, 1, -2, -2};
  int xs[10];
  int ys[10];
  for (int i = 0; i < 10; i++) {
    xs[i] = centerX + REL_X[i];
    ys[i] = centerY + REL_Y[i];
  }
  if (filled) {
    renderer.fillPolygon(xs, ys, 10, black);
    return;
  }
  for (int i = 0; i < 10; i++) renderer.drawLine(xs[i], ys[i], xs[(i + 1) % 10], ys[(i + 1) % 10], black);
}

void drawRating(const GfxRenderer& renderer, const int x, const int y, const uint8_t rating, const bool black) {
  for (int star = 0; star < 5; star++) drawMiniStar(renderer, x + star * 16 + 6, y + 6, star < rating, black);
}

void formatSeries(const RecentBook& book, char* output, const size_t outputSize) {
  if (!output || outputSize == 0) return;
  output[0] = '\0';
  if (book.series.empty()) return;
  if (book.seriesPosition == 0) {
    snprintf(output, outputSize, "%s", book.series.c_str());
    return;
  }
  char position[12];
  series_metadata::formatPosition(book.seriesPosition, position, sizeof(position));
  snprintf(output, outputSize, tr(STR_SERIES_BOOK_FORMAT), book.series.c_str(), position);
}

bool drawCover(const GfxRenderer& renderer, const RecentBook& book, const int x, const int y, const int width,
               const int height, const bool selected) {
  renderer.fillRect(x, y, width, height, false);
  bool drawn = false;
  if (!book.coverBmpPath.empty()) {
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, height);
    drawn = UITheme::drawCoverThumbFilled(const_cast<GfxRenderer&>(renderer), coverPath, x, y, width, height,
                                          /*allowRawDecode=*/false);
  }
  renderer.drawRect(x, y, width, height, !selected);
  if (drawn) return true;

  renderer.drawIcon(CoverIcon, x + (width - 32) / 2, y + std::max(8, (height - 32) / 3), 32);
  const auto lines = renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), width - 10, 3, EpdFontFamily::BOLD);
  int textY = y + height / 2 + 12;
  for (const auto& line : lines) {
    if (textY + renderer.getLineHeight(SMALL_FONT_ID) >= y + height) break;
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
    renderer.drawText(SMALL_FONT_ID, x + (width - textWidth) / 2, textY, line.c_str(), true, EpdFontFamily::BOLD);
    textY += renderer.getLineHeight(SMALL_FONT_ID);
  }
  return false;
}

void drawBookTile(const GfxRenderer& renderer, const RecentBook& book, const uint8_t rating, const int x, const int y,
                  const int width, const int height, const bool selected, const bool completed) {
  if (selected) renderer.fillRect(x, y, width, height, true);

  constexpr int tilePadding = 5;
  const int coverWidth = width - tilePadding * 2;
  const bool hasSeries = !book.series.empty();
  const int metadataHeight = completed && rating > 0 && hasSeries ? 58 : 42;
  const int coverHeight = std::min(height - metadataHeight, coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM);
  const int coverX = x + tilePadding;
  const int coverY = y + tilePadding;
  drawCover(renderer, book, coverX, coverY, coverWidth, coverHeight, selected);

  if (completed) {
    constexpr int badgeSize = 20;
    const int badgeX = coverX + coverWidth - badgeSize - 4;
    const int badgeY = coverY + coverHeight - badgeSize - 4;
    renderer.fillRoundedRect(badgeX, badgeY, badgeSize, badgeSize, badgeSize / 2,
                             selected ? Color::White : Color::Black);
    const bool markBlack = selected;
    renderer.drawLine(badgeX + 5, badgeY + 10, badgeX + 9, badgeY + 14, 2, markBlack);
    renderer.drawLine(badgeX + 9, badgeY + 14, badgeX + 16, badgeY + 6, 2, markBlack);
  }

  const bool ink = !selected;
  const int textY = coverY + coverHeight + 5;
  const auto title = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), coverWidth, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, coverX, textY, title.c_str(), ink, EpdFontFamily::BOLD);

  char series[96];
  formatSeries(book, series, sizeof(series));
  int metadataY = textY + renderer.getLineHeight(SMALL_FONT_ID);
  if (series[0] != '\0') {
    const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, series, coverWidth);
    renderer.drawText(SMALL_FONT_ID, coverX, metadataY, subtitle.c_str(), ink);
    metadataY += renderer.getLineHeight(SMALL_FONT_ID);
  }
  if (completed && rating > 0) drawRating(renderer, coverX, metadataY + 1, rating, ink);
}

void drawBookRow(const GfxRenderer& renderer, const Rect rect, const RecentBook* books, const uint8_t* ratings,
                 const int start, const int count, const int selectionOffset, const int selectedIndex,
                 const bool completed) {
  const int cellWidth = (rect.width - GRID_GAP * (GRID_COLUMNS - 1)) / GRID_COLUMNS;
  for (int column = 0; column < count; column++) {
    const int bookIndex = start + column;
    const int x = rect.x + column * (cellWidth + GRID_GAP);
    const uint8_t rating = ratings ? ratings[bookIndex] : 0;
    drawBookTile(renderer, books[bookIndex], rating, x, rect.y, cellWidth, rect.height,
                 selectedIndex == selectionOffset + bookIndex, completed);
  }
}

void drawNow(const GfxRenderer& renderer, const Rect rect, const ReadingHubScreen& screen) {
  renderer.drawText(UI_10_FONT_ID, rect.x, rect.y, tr(STR_CONTINUE_READING), true, EpdFontFamily::BOLD);
  const int cardY = rect.y + renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int cardHeight = std::min(286, rect.height * 45 / 100);
  const bool selected = screen.selectedIndex == 0;
  if (selected) {
    renderer.fillRoundedRect(rect.x, cardY, rect.width, cardHeight, CARD_RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(rect.x, cardY, rect.width, cardHeight, 2, CARD_RADIUS, true);
  }

  if (screen.currentBook) {
    const int coverHeight = cardHeight - 22;
    const int coverWidth = coverHeight * COVER_ASPECT_NUM / COVER_ASPECT_DEN;
    drawCover(renderer, *screen.currentBook, rect.x + 11, cardY + 11, coverWidth, coverHeight, selected);

    const int textX = rect.x + coverWidth + 27;
    const int textWidth = rect.width - coverWidth - 40;
    const bool ink = !selected;
    const auto titleLines = renderer.wrappedText(NOTOSERIF_18_FONT_ID, screen.currentBook->title.c_str(), textWidth, 2,
                                                 EpdFontFamily::BOLD);
    int textY = cardY + 14;
    for (const auto& line : titleLines) {
      renderer.drawText(NOTOSERIF_18_FONT_ID, textX, textY, line.c_str(), ink, EpdFontFamily::BOLD);
      textY += renderer.getLineHeight(NOTOSERIF_18_FONT_ID);
    }
    if (!screen.currentBook->author.empty()) {
      const auto author = renderer.truncatedText(SMALL_FONT_ID, screen.currentBook->author.c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, textX, textY + 3, author.c_str(), ink);
      textY += renderer.getLineHeight(SMALL_FONT_ID) + 3;
    }
    char series[96];
    formatSeries(*screen.currentBook, series, sizeof(series));
    if (series[0] != '\0') {
      const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, series, textWidth);
      renderer.drawText(SMALL_FONT_ID, textX, textY + 3, subtitle.c_str(), ink);
    }

    const int progressY = cardY + cardHeight - 64;
    renderer.drawRect(textX, progressY, textWidth, 9, ink);
    if (screen.currentProgress >= 0) {
      const int fill = std::clamp(screen.currentProgress, 0, 100) * (textWidth - 4) / 100;
      if (fill > 0) renderer.fillRect(textX + 2, progressY + 2, fill, 5, ink);
    }
    char progressText[24];
    if (screen.currentProgress >= 0) {
      snprintf(progressText, sizeof(progressText), "%d%%", screen.currentProgress);
      renderer.drawText(SMALL_FONT_ID, textX, progressY + 14, progressText, ink);
    }
    renderer.drawText(UI_10_FONT_ID, textX, cardY + cardHeight - 26, tr(STR_RESUME), ink, EpdFontFamily::BOLD);
  } else {
    UITheme::drawCenteredWrappedText(renderer, Rect{rect.x + 18, cardY + 18, rect.width - 36, cardHeight - 36},
                                     UI_12_FONT_ID, tr(STR_NO_OPEN_BOOK), 2, !selected, EpdFontFamily::BOLD);
  }

  const int queueY = cardY + cardHeight + 14;
  const int queueHeight = 104;
  const bool queueSelected = screen.selectedIndex == 1;
  if (queueSelected) {
    renderer.fillRect(rect.x, queueY, rect.width, queueHeight, true);
  } else {
    renderer.drawRect(rect.x, queueY, rect.width, queueHeight, 2, true);
  }
  const bool queueInk = !queueSelected;
  if (screen.nextBook) {
    const int coverHeight = queueHeight - 18;
    const int coverWidth = coverHeight * COVER_ASPECT_NUM / COVER_ASPECT_DEN;
    drawCover(renderer, *screen.nextBook, rect.x + 10, queueY + 9, coverWidth, coverHeight, queueSelected);
    const int textX = rect.x + coverWidth + 24;
    renderer.drawText(SMALL_FONT_ID, textX, queueY + 9, tr(STR_HUB_UP_NEXT), queueInk, EpdFontFamily::BOLD);
    const auto title = renderer.truncatedText(UI_12_FONT_ID, screen.nextBook->title.c_str(),
                                              rect.width - coverWidth - 55, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX, queueY + 31, title.c_str(), queueInk, EpdFontFamily::BOLD);
    char series[96];
    formatSeries(*screen.nextBook, series, sizeof(series));
    const char* subtitleText = series[0] == '\0' ? screen.nextBook->author.c_str() : series;
    const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText, rect.width - coverWidth - 55);
    renderer.drawText(SMALL_FONT_ID, textX, queueY + 65, subtitle.c_str(), queueInk);
  } else {
    renderer.drawText(UI_12_FONT_ID, rect.x + 15, queueY + 20, tr(STR_HUB_OPEN_QUEUE), queueInk, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, rect.x + 15, queueY + 54, tr(STR_HUB_QUEUE_EMPTY), queueInk);
  }
  drawChevron(renderer, rect.x + rect.width - 15, queueY + queueHeight / 2, queueInk);

  const int achievementY = queueY + queueHeight + 18;
  renderer.drawLine(rect.x, achievementY, rect.x + rect.width, achievementY, true);
  char completed[48];
  char queued[48];
  snprintf(completed, sizeof(completed), tr(STR_HUB_COMPLETED_COUNT), screen.completedTotalCount);
  snprintf(queued, sizeof(queued), tr(STR_HUB_QUEUE_COUNT), screen.queueTotalCount);
  renderer.drawText(UI_10_FONT_ID, rect.x + 4, achievementY + 14, completed, true, EpdFontFamily::BOLD);
  const int queuedWidth = renderer.getTextWidth(SMALL_FONT_ID, queued);
  renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - queuedWidth - 4, achievementY + 17, queued, true);
  if (screen.selectedIndex == 2) renderer.invertRect(rect.x, achievementY + 7, rect.width, 35);
}

void drawLibrary(const GfxRenderer& renderer, const Rect rect, const ReadingHubScreen& screen) {
  renderer.drawText(UI_10_FONT_ID, rect.x, rect.y, tr(STR_HUB_YOUR_SHELVES), true, EpdFontFamily::BOLD);
  char libraryStatus[48];
  snprintf(libraryStatus, sizeof(libraryStatus), tr(STR_HUB_LIBRARY_COUNT),
           static_cast<unsigned>(screen.libraryTotalCount));
  const int statusWidth = renderer.getTextWidth(SMALL_FONT_ID, libraryStatus);
  renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - statusWidth, rect.y + 2, libraryStatus);

  if (!screen.shelves || screen.shelfPreviewCount <= 0) {
    const bool selected = screen.selectedIndex == 0;
    const int emptyY = rect.y + 48;
    renderer.drawRoundedRect(rect.x, emptyY, rect.width, 86, 2, CARD_RADIUS, true);
    renderer.drawIcon(FolderIcon, rect.x + 16, emptyY + 27, 32);
    renderer.drawText(UI_12_FONT_ID, rect.x + 62, emptyY + 19, tr(STR_HUB_OPEN_LIBRARY), true, EpdFontFamily::BOLD);
    const char* detail = screen.shelfSummaryAvailable ? tr(STR_NO_ACTIVE_BOOKS) : tr(STR_HUB_SHELF_SUMMARY_UNAVAILABLE);
    renderer.drawText(SMALL_FONT_ID, rect.x + 62, emptyY + 51, detail);
    drawChevron(renderer, rect.x + rect.width - 16, emptyY + 43, true);
    if (selected) renderer.invertRect(rect.x, emptyY, rect.width, 86);
    return;
  }

  const bool hasAllRow = screen.shelfTotalCount > screen.shelfPreviewCount;
  const int rowCount = screen.shelfPreviewCount + (hasAllRow ? 1 : 0);
  const int top = rect.y + 34;
  const int rowHeight = std::min(88, std::max(64, (rect.height - 34) / std::max(1, rowCount)));
  for (int index = 0; index < screen.shelfPreviewCount; index++) {
    const auto& shelf = screen.shelves[index];
    const int y = top + index * rowHeight;
    const bool selected = screen.selectedIndex == index;
    renderer.drawLine(rect.x, y + rowHeight - 1, rect.x + rect.width, y + rowHeight - 1, true);

    constexpr int coverWidth = 44;
    const int coverHeight = std::min(66, rowHeight - 10);
    const int coverY = y + (rowHeight - coverHeight) / 2;
    bool coverDrawn = false;
    if (!shelf.coverBmpPath.empty()) {
      coverDrawn = UITheme::drawCoverThumbFilled(const_cast<GfxRenderer&>(renderer), shelf.coverBmpPath, rect.x + 6,
                                                 coverY, coverWidth, coverHeight, false);
    }
    renderer.drawRect(rect.x + 6, coverY, coverWidth, coverHeight, true);
    if (!coverDrawn) renderer.drawIcon(FolderIcon, rect.x + 12, coverY + std::max(3, (coverHeight - 32) / 2), 32);

    const int textX = rect.x + 64;
    const auto title = renderer.truncatedText(UI_12_FONT_ID, shelf.name.c_str(), rect.width - 104, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX, y + 13, title.c_str(), true, EpdFontFamily::BOLD);
    char count[40];
    snprintf(count, sizeof(count), tr(STR_HUB_SHELF_BOOK_COUNT), static_cast<unsigned>(shelf.bookCount));
    renderer.drawText(SMALL_FONT_ID, textX, y + 45, count);
    drawChevron(renderer, rect.x + rect.width - 13, y + rowHeight / 2, true);
    if (selected) renderer.invertRect(rect.x, y, rect.width, rowHeight);
  }
  if (!hasAllRow) return;
  const int y = top + screen.shelfPreviewCount * rowHeight;
  renderer.drawText(UI_12_FONT_ID, rect.x + 16, y + 17, tr(STR_HUB_SHOW_ALL_SHELVES), true, EpdFontFamily::BOLD);
  drawChevron(renderer, rect.x + rect.width - 13, y + rowHeight / 2, true);
  if (screen.selectedIndex == screen.shelfPreviewCount) renderer.invertRect(rect.x, y, rect.width, rowHeight);
}

void drawQueue(const GfxRenderer& renderer, const Rect rect, const ReadingHubScreen& screen) {
  renderer.drawText(UI_10_FONT_ID, rect.x, rect.y, tr(STR_HUB_READING_ORDER), true, EpdFontFamily::BOLD);
  char total[48];
  snprintf(total, sizeof(total), tr(STR_HUB_QUEUE_COUNT), screen.queueTotalCount);
  const int totalWidth = renderer.getTextWidth(SMALL_FONT_ID, total);
  renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - totalWidth, rect.y + 2, total);
  renderer.drawText(SMALL_FONT_ID, rect.x, rect.y + 22, tr(STR_HUB_HOLD_TO_REORDER));

  if (!screen.queueBooks || screen.queuePreviewCount <= 0) {
    UITheme::drawCenteredWrappedText(renderer, Rect{rect.x + 20, rect.y + 70, rect.width - 40, rect.height - 100},
                                     UI_12_FONT_ID, tr(STR_READING_QUEUE_EMPTY), 3, true, EpdFontFamily::BOLD);
    return;
  }

  const int top = rect.y + 45;
  const int rowHeight = std::min(112, (rect.height - 45) / screen.queuePreviewCount);
  for (int index = 0; index < screen.queuePreviewCount; index++) {
    const int y = top + index * rowHeight;
    const bool selected = index == screen.selectedIndex;
    if (selected) renderer.fillRect(rect.x, y, rect.width, rowHeight, true);
    renderer.drawLine(rect.x, y, rect.x + rect.width, y, !selected);
    if (index + 1 == screen.queuePreviewCount) {
      renderer.drawLine(rect.x, y + rowHeight - 1, rect.x + rect.width, y + rowHeight - 1, !selected);
    }

    char rank[4];
    snprintf(rank, sizeof(rank), "%02d", index + 1);
    renderer.drawText(UI_12_FONT_ID, rect.x + 7, y + rowHeight / 2 - 12, rank, !selected, EpdFontFamily::BOLD);

    const int coverHeight = rowHeight - 16;
    const int coverWidth = coverHeight * COVER_ASPECT_NUM / COVER_ASPECT_DEN;
    const int coverX = rect.x + 48;
    drawCover(renderer, screen.queueBooks[index], coverX, y + 8, coverWidth, coverHeight, selected);
    const int textX = coverX + coverWidth + 13;
    const int textWidth = rect.width - (textX - rect.x) - 28;
    const auto title =
        renderer.truncatedText(UI_12_FONT_ID, screen.queueBooks[index].title.c_str(), textWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX, y + 20, title.c_str(), !selected, EpdFontFamily::BOLD);
    char series[96];
    formatSeries(screen.queueBooks[index], series, sizeof(series));
    const char* subtitleText = series[0] == '\0' ? screen.queueBooks[index].author.c_str() : series;
    const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText, textWidth);
    renderer.drawText(SMALL_FONT_ID, textX, y + 53, subtitle.c_str(), !selected);
    renderer.drawText(UI_12_FONT_ID, rect.x + rect.width - 20, y + rowHeight / 2 - 12, "=", !selected,
                      EpdFontFamily::BOLD);
  }
}

void drawRead(const GfxRenderer& renderer, const Rect rect, const ReadingHubScreen& screen) {
  constexpr int summaryHeight = 78;
  renderer.drawLine(rect.x, rect.y + summaryHeight - 1, rect.x + rect.width, rect.y + summaryHeight - 1, true);
  const int third = rect.width / 3;
  char completed[12];
  char rated[12];
  char average[12];
  snprintf(completed, sizeof(completed), "%d", screen.completedTotalCount);
  snprintf(rated, sizeof(rated), "%d", screen.ratedBookCount);
  snprintf(average, sizeof(average), "%d.%d", screen.averageRatingTenths / 10, screen.averageRatingTenths % 10);
  UITheme::drawCenteredText(renderer, Rect{rect.x, rect.y, third, summaryHeight}, NOTOSANS_18_FONT_ID, rect.y + 2,
                            completed, true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, Rect{rect.x + third, rect.y, third, summaryHeight}, NOTOSANS_18_FONT_ID,
                            rect.y + 2, rated, true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, Rect{rect.x + third * 2, rect.y, rect.width - third * 2, summaryHeight},
                            NOTOSANS_18_FONT_ID, rect.y + 2, average, true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, Rect{rect.x, rect.y, third, summaryHeight}, SMALL_FONT_ID, rect.y + 43,
                            tr(STR_HUB_COMPLETED_LABEL));
  UITheme::drawCenteredText(renderer, Rect{rect.x + third, rect.y, third, summaryHeight}, SMALL_FONT_ID, rect.y + 43,
                            tr(STR_HUB_RATED_LABEL));
  UITheme::drawCenteredText(renderer, Rect{rect.x + third * 2, rect.y, rect.width - third * 2, summaryHeight},
                            SMALL_FONT_ID, rect.y + 43, tr(STR_HUB_AVERAGE_LABEL));

  constexpr int showAllHeight = 50;
  const int showAllY = rect.y + summaryHeight + 10;
  renderer.drawRoundedRect(rect.x, showAllY, rect.width, showAllHeight, 2, CARD_RADIUS, true);
  renderer.drawText(UI_12_FONT_ID, rect.x + 15, showAllY + 12, tr(STR_HUB_SHOW_ALL_COMPLETED), true,
                    EpdFontFamily::BOLD);
  drawChevron(renderer, rect.x + rect.width - 14, showAllY + showAllHeight / 2, true);
  if (screen.selectedIndex == 0) renderer.invertRect(rect.x, showAllY, rect.width, showAllHeight);

  const int groupY = showAllY + showAllHeight + 16;
  renderer.drawText(UI_10_FONT_ID, rect.x, groupY, tr(STR_HUB_RECENTLY_COMPLETED), true, EpdFontFamily::BOLD);
  const int rateHintWidth = renderer.getTextWidth(SMALL_FONT_ID, tr(STR_HUB_HOLD_TO_RATE));
  renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - rateHintWidth, groupY + 2, tr(STR_HUB_HOLD_TO_RATE));

  if (!screen.completedBooks || screen.completedPreviewCount <= 0) {
    UITheme::drawCenteredWrappedText(
        renderer, Rect{rect.x + 20, groupY + 38, rect.width - 40, rect.height - (groupY - rect.y) - 58}, UI_12_FONT_ID,
        tr(STR_NO_COMPLETED_BOOKS), 3, true, EpdFontFamily::BOLD);
    return;
  }

  const int firstCount = std::min(GRID_COLUMNS, screen.completedPreviewCount);
  const int gridTop = groupY + 25;
  const int rowHeight = std::max(170, (rect.y + rect.height - gridTop - 10) / 2);
  const Rect firstRow{rect.x, gridTop, rect.width, rowHeight};
  drawBookRow(renderer, firstRow, screen.completedBooks, screen.completedRatings, 0, firstCount, 1,
              screen.selectedIndex, true);

  const int remaining = screen.completedPreviewCount - firstCount;
  if (remaining <= 0) return;
  const Rect secondRow{rect.x, firstRow.y + firstRow.height + 9, rect.width, rowHeight};
  drawBookRow(renderer, secondRow, screen.completedBooks, screen.completedRatings, firstCount,
              std::min(GRID_COLUMNS, remaining), 1, screen.selectedIndex, true);
}
}  // namespace

void ReadingHubTheme::drawReadingHubScreen(const GfxRenderer& renderer, const Rect rect,
                                           const ReadingHubScreen& screen) const {
  if (rect.width <= 0 || rect.height <= 0) return;
  switch (screen.section) {
    case 0:
      drawNow(renderer, rect, screen);
      break;
    case 1:
      drawLibrary(renderer, rect, screen);
      break;
    case 2:
      drawQueue(renderer, rect, screen);
      break;
    case 3:
      drawRead(renderer, rect, screen);
      break;
    default:
      break;
  }
}
