#include "BookActionsActivity.h"

#include <I18n.h>

#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

BookActionsActivity::BookActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                         const bool queued, const bool finished, const int queueIndex,
                                         const int queueCount)
    : UiListActivity("BookActions", renderer, mappedInput), title_(std::move(title)) {
  addAction(BookAction::VIEW_STATS, StrId::STR_VIEW_READING_STATS, UIIcon::Stats);
  if (!finished) {
    addAction(queued ? BookAction::REMOVE_FROM_QUEUE : BookAction::ADD_TO_QUEUE,
              queued ? StrId::STR_REMOVE_FROM_READING_QUEUE : StrId::STR_ADD_TO_READING_QUEUE, UIIcon::Bookmark);
  }
  addAction(finished ? BookAction::MARK_UNFINISHED : BookAction::MARK_COMPLETED,
            finished ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_COMPLETED, UIIcon::Book);
  if (queued && queueIndex > 0) addAction(BookAction::MOVE_EARLIER, StrId::STR_MOVE_EARLIER, UIIcon::Bookmark);
  if (queued && queueIndex + 1 < queueCount) {
    addAction(BookAction::MOVE_LATER, StrId::STR_MOVE_LATER, UIIcon::Bookmark);
  }
}

void BookActionsActivity::addAction(const BookAction action, const StrId label, const UIIcon icon) {
  if (rowCount_ >= MAX_ACTIONS) return;
  fui::ListItem item;
  item.label = I18N.get(label);
  item.icon = listIconFor(icon);
  item.actionValue = static_cast<int16_t>(rowCount_);
  actions_[rowCount_] = action;
  rows_[rowCount_++] = item;
}

void BookActionsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rows_.data();
  props.count = static_cast<uint16_t>(rowCount_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void BookActionsActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount_) return;
  app.clearTapFlash();
  setResult(IntervalResult{static_cast<uint32_t>(actions_[index])});
  finish();
}

void BookActionsActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
