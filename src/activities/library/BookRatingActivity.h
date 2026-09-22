#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Small, button-first 1..5 star picker shared by Library Book Actions and the
// end-of-book screen. Stars are drawn as geometry so the UI does not depend on
// a particular SD font containing the Unicode star glyphs.
class BookRatingActivity final : public Activity {
 public:
  BookRatingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                     uint8_t initialRating, bool readerActivity = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerActivity_; }

 private:
  static constexpr int STAR_COUNT = 5;

  std::string title_;
  uint8_t rating_;
  bool readerActivity_;
  ButtonNavigator buttonNavigator_;

  void adjust(int delta);
  void confirm();
  void cancel();
  void drawStar(int centerX, int centerY, bool filled) const;
};
