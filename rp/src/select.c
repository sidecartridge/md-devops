#include "select.h"

static reset_callback_t reset_cb = NULL;
static reset_callback_t reset_long_cb = NULL;
static bool selectPressedLatched = false;
static absolute_time_t selectPressStartTime;
static bool selectLongPressDetected = false;

// Stronger debouncer ported from md-drives-emulator: poll the button
// every SELECT_LOOP_DELAY ms and require SELECT_DEBOUNCE_MS of
// continuously matching samples before accepting the state. Replaces a
// 2-sample debouncer that could be fooled by a single bouncing edge
// during the 20 ms window.
static bool select_detectStableState(bool expectedState) {
  uint32_t stable_ms = 0;
  while (stable_ms < SELECT_DEBOUNCE_MS) {
    if (select_detectPush() != expectedState) {
      return false;
    }
    tight_loop_contents();
    sleep_ms(SELECT_LOOP_DELAY);
    stable_ms += SELECT_LOOP_DELAY;
  }
  return true;
}

static uint32_t select_getPressDurationMs(void) {
  int64_t elapsedUs =
      absolute_time_diff_us(selectPressStartTime, get_absolute_time());
  if (elapsedUs <= 0) {
    return 0;
  }

  return (uint32_t)(elapsedUs / 1000);
}

void select_configure() {
  // Configure the input ping for SELECT button
  gpio_init(SELECT_GPIO);
  gpio_set_dir(SELECT_GPIO, GPIO_IN);
  gpio_set_pulls(SELECT_GPIO, false, true);  // Pull down (false, true)
  gpio_pull_down(SELECT_GPIO);
}

bool select_detectPush() { return (gpio_get(SELECT_GPIO) != 0); }

// The core-1 SELECT watcher lived here: select_waitPush(),
// select_coreWaitPush() and select_coreWaitPushDisable(). Removed in v1.1.
// Nothing called them, and core 1 had already been backed out earlier because running it froze the Wi-Fi poll loop the main loop depends on.
// Using it again would also need Booster's flash lockout (its settings.c grew
// select_flashLockoutBegin once core 1 executed from flash during an erase), so
// a ready-made entry point into a known hazard was worth deleting rather than
// leaving for someone to find. SELECT is polled in the foreground by
// select_checkPushReset() below.
void select_checkPushReset() {
  bool isPressed = select_detectPush();
  if (isPressed && !selectPressedLatched) {
    if (!select_detectStableState(true)) {
      return;
    }

    selectPressedLatched = true;
    selectPressStartTime = get_absolute_time();
    selectLongPressDetected = false;
    DPRINTF("SELECT button pushed. Waiting for release\n");
    return;
  }

  if (isPressed && selectPressedLatched) {
    if (!selectLongPressDetected &&
        (select_getPressDurationMs() >= SELECT_LONG_RESET)) {
      selectLongPressDetected = true;
      DPRINTF("SELECT button long press threshold reached\n");
    }
    return;
  }

  if (!isPressed && selectPressedLatched) {
    if (!select_detectStableState(false)) {
      return;
    }

    uint32_t pressDurationMs = select_getPressDurationMs();
    bool longPress = selectLongPressDetected ||
                     (pressDurationMs >= SELECT_LONG_RESET);
    selectPressedLatched = false;
    selectLongPressDetected = false;

    DPRINTF("SELECT button released after %lu ms\n",
            (unsigned long)pressDurationMs);
    if (longPress) {
      if (reset_long_cb != NULL) {
        DPRINTF("Long press detected. Executing long reset callback\n");
        reset_long_cb();
      }
    } else {
      if (reset_cb != NULL) {
        DPRINTF("Short press detected. Executing reset callback\n");
        reset_cb();
      }
    }
  }
}

void select_setResetCallback(reset_callback_t reset) { reset_cb = reset; }
void select_setLongResetCallback(reset_callback_t resetLong) {
  reset_long_cb = resetLong;
}
