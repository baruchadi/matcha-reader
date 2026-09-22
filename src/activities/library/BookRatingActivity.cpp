#include "BookRatingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int STAR_RADIUS = 22;
constexpr int STAR_STEP = 50;
constexpr int STAR_TOUCH_HEIGHT = 64;
}  // namespace

BookRatingActivity::BookRatingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                       const uint8_t initialRating, const bool readerActivity)
    : Activity("BookRating", renderer, mappedInput),
      title_(std::move(title)),
      rating_(initialRating >= 1 && initialRating <= STAR_COUNT ? initialRating : 3),
      readerActivity_(readerActivity) {}

void BookRatingActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void BookRatingActivity::adjust(const int delta) {
  const uint8_t next = static_cast<uint8_t>(std::clamp(static_cast<int>(rating_) + delta, 1, STAR_COUNT));
  if (next == rating_) return;
  rating_ = next;
  requestUpdate();
}

void BookRatingActivity::confirm() {
  setResult(IntervalResult{rating_});
  finish();
}

void BookRatingActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BookRatingActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator_.onPrevious([this] { adjust(-1); });
  buttonNavigator_.onNext([this] { adjust(1); });

  const int starsWidth = STAR_COUNT * STAR_STEP;
  const int starsLeft = (renderer.getScreenWidth() - starsWidth) / 2;
  const int starsY = renderer.getScreenHeight() / 2;
  int touched = -1;
  const auto touch = mappedInput.colTouch(touched, starsLeft, STAR_STEP, STAR_COUNT,
                                          starsY - STAR_TOUCH_HEIGHT / 2, starsY + STAR_TOUCH_HEIGHT / 2, STAR_STEP);
  if (touch == MappedInputManager::RowTouch::Down && touched >= 0) {
    const uint8_t next = static_cast<uint8_t>(touched + 1);
    if (next != rating_) {
      rating_ = next;
      requestUpdate();
    }
  } else if (touch == MappedInputManager::RowTouch::Tap && touched >= 0) {
    rating_ = static_cast<uint8_t>(touched + 1);
    confirm();
  }
}

void BookRatingActivity::drawStar(const int centerX, const int centerY, const bool filled) const {
  static constexpr int REL_X[10] = {0, 6, 21, 9, 13, 0, -13, -9, -21, -6};
  static constexpr int REL_Y[10] = {-22, -7, -7, 3, 18, 9, 18, 3, -7, -7};
  int xs[10];
  int ys[10];
  for (int i = 0; i < 10; i++) {
    xs[i] = centerX + REL_X[i] * STAR_RADIUS / 22;
    ys[i] = centerY + REL_Y[i] * STAR_RADIUS / 22;
  }
  if (filled) {
    renderer.fillPolygon(xs, ys, 10, true);
    return;
  }
  for (int i = 0; i < 10; i++) renderer.drawLine(xs[i], ys[i], xs[(i + 1) % 10], ys[(i + 1) % 10], true);
}

void BookRatingActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 title_.c_str());

  const int promptY = metrics.topPadding + metrics.headerHeight + renderer.getScreenHeight() / 7;
  renderer.drawCenteredText(UI_12_FONT_ID, promptY, tr(STR_RATE_THIS_BOOK), true, EpdFontFamily::BOLD);

  const int starsWidth = STAR_COUNT * STAR_STEP;
  const int starsLeft = (renderer.getScreenWidth() - starsWidth) / 2;
  const int starsY = renderer.getScreenHeight() / 2;
  for (int i = 0; i < STAR_COUNT; i++) drawStar(starsLeft + i * STAR_STEP + STAR_STEP / 2, starsY, i < rating_);

  char value[32];
  snprintf(value, sizeof(value), tr(STR_STAR_RATING_FORMAT), static_cast<unsigned int>(rating_));
  renderer.drawCenteredText(UI_10_FONT_ID, starsY + STAR_RADIUS + metrics.verticalSpacing * 3, value);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SAVE), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
