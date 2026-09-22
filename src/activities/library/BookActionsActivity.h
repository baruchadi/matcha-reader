#pragma once

#include <I18n.h>

#include <array>
#include <cstdint>
#include <string>

#include "activities/UiListActivity.h"
#include "components/UITheme.h"

enum class BookAction : uint32_t {
  VIEW_STATS,
  ADD_TO_QUEUE,
  REMOVE_FROM_QUEUE,
  MARK_COMPLETED,
  MARK_UNFINISHED,
  RATE_BOOK,
  MOVE_EARLIER,
  MOVE_LATER,
};

class BookActionsActivity final : public UiListActivity {
 public:
  BookActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, bool queued,
                      bool finished, int queueIndex, int queueCount);

 private:
  static constexpr int MAX_ACTIONS = 6;
  std::string title_;
  std::array<freeink::ui::ListItem, MAX_ACTIONS> rows_{};
  std::array<BookAction, MAX_ACTIONS> actions_{};
  int rowCount_ = 0;

  void addAction(BookAction action, StrId label, UIIcon icon);
  int listCount() const override { return rowCount_; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return title_.c_str(); }
};
