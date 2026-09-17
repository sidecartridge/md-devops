/**
 * File: select.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2025, February 2026
 * Copyright: 2024-2026 - GOODDATA LABS SL
 * Description: Header file for SELECT button detection functions
 */

#ifndef SELECT_H
#define SELECT_H

#include "constants.h"
#include "debug.h"
#include "pico/stdlib.h"

#define SELECT_LOOP_DELAY 10  // 10 ms

// Window of stable level required before accepting a press/release. The
// debouncer polls every SELECT_LOOP_DELAY and requires SELECT_DEBOUNCE_MS
// of consistent readings before returning true.
#define SELECT_DEBOUNCE_MS 30

#define SELECT_LONG_RESET 10000  // 10 seconds

// Define a callback typdef for the reset function
typedef void (*reset_callback_t)(void);

/**
 * @brief Initializes the SELECT detection.
 *
 * Configures the hardware and software parameters needed for detecting
 * the SELECT button press. Always call first.
 */
void select_configure();



/**
 * @brief Detects button press.
 *
 * Checks whether the SELECT button has been pressed.
 *
 * Verifies the button state and returns true if a push is detected.
 */
bool select_detectPush();





/**
 * @brief Monitors for reset trigger.
 *
 * Monitors for a SELECT button push intended to trigger a device reset.
 *
 * Frequently called to check for reset conditions during operation.
 */
void select_checkPushReset();

/**
 * @brief Registers the reset callback.
 *
 * Registers a callback to be invoked when a reset is requested by the
 * SELECT button event.
 *
 * Associates a function that will perform the necessary reset actions.
 */
void select_setResetCallback(reset_callback_t reset);

/**
 * @brief Registers the long reset callback.
 *
 * Associates a function that will be invoked when a long press reset is
 * requested by a prolonged SELECT button push.
 *
 * @param resetLong Callback function for a long press reset action.
 */
void select_setLongResetCallback(reset_callback_t resetLong);

#endif  // SELECT_H
