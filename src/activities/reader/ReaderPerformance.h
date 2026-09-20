#pragma once

#include <cstdint>

constexpr uint32_t READER_OPEN_BOOKKEEPING_IDLE_MS = 400;

inline bool readerBackgroundWorkAllowed(const bool buttonDownRaw, const bool pressed, const bool released,
                                        const bool renderPending) {
  return !buttonDownRaw && !pressed && !released && !renderPending;
}
