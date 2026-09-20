#include <gtest/gtest.h>

#include "src/LibraryBookInput.h"

namespace {

struct FakeInput {
  enum class Button { Confirm, Power };

  bool confirmDown = false;
  bool confirmReleased = false;
  bool powerReleased = false;
  bool confirmLongPressLatched = false;
  bool confirmReleaseSuppressed = false;
  unsigned long heldMs = 0;

  bool wasLongPressed(const Button button, const unsigned long thresholdMs) {
    if (button != Button::Confirm || !confirmDown || heldMs < thresholdMs || confirmLongPressLatched) return false;
    confirmLongPressLatched = true;
    confirmReleaseSuppressed = true;
    return true;
  }

  bool wasReleased(const Button button) const {
    if (button == Button::Power) return powerReleased;
    return confirmReleased && !confirmReleaseSuppressed;
  }

  unsigned long getHeldTime() const { return heldMs; }
};

}  // namespace

TEST(LibraryBookInput, SelectHoldOpensActionsAndItsReleaseCannotActivateTheNewScreen) {
  FakeInput input;
  input.confirmDown = true;
  input.heldMs = 700;

  EXPECT_EQ(pollLibraryBookInput(input, 700, false), LibraryBookInputAction::ShowActions);

  input.confirmDown = false;
  input.confirmReleased = true;
  EXPECT_FALSE(input.wasReleased(FakeInput::Button::Confirm));
}

TEST(LibraryBookInput, SelectClickStillOpensTheFocusedBook) {
  FakeInput input;
  input.confirmReleased = true;
  input.heldMs = 120;

  EXPECT_EQ(pollLibraryBookInput(input, 700, false), LibraryBookInputAction::OpenBook);
}

TEST(LibraryBookInput, PowerClickOpensActionsOnlyWhenThatShortcutIsConfigured) {
  FakeInput input;
  input.powerReleased = true;

  EXPECT_EQ(pollLibraryBookInput(input, 700, false), LibraryBookInputAction::None);
  EXPECT_EQ(pollLibraryBookInput(input, 700, true), LibraryBookInputAction::ShowActions);
}
