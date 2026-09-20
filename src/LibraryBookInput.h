#pragma once

#include <cstdint>

enum class LibraryBookInputAction : uint8_t { None, OpenBook, ShowActions };

// Resolve the controls for a focused book in any Library grid. Calling the input manager's
// one-shot long-press API is important: it arms global release suppression before the actions
// screen is pushed, so the release ending this hold cannot activate that screen's first row.
template <typename Input>
LibraryBookInputAction pollLibraryBookInput(Input& input, const unsigned long confirmHoldMs,
                                            const bool powerOpensActions) {
  using Button = typename Input::Button;
  if (input.wasLongPressed(Button::Confirm, confirmHoldMs)) return LibraryBookInputAction::ShowActions;
  if (powerOpensActions && input.wasReleased(Button::Power)) return LibraryBookInputAction::ShowActions;
  if (input.wasReleased(Button::Confirm) && input.getHeldTime() < confirmHoldMs) {
    return LibraryBookInputAction::OpenBook;
  }
  return LibraryBookInputAction::None;
}
