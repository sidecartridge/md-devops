/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

#include <stdbool.h>
#include <stdint.h>

#include "runner.h"

/**
 * @brief
 *
 * Launches the ROM emulator application. Initializes terminal interfaces,
 * configures network and storage systems, and loads the ROM data from SD or
 * network sources. Manages the main loop which includes firmware bypass,
 * user interaction and potential system resets.
 */
void emul_start();

/**
 * @brief Whether the user picked Runner mode at boot ([U] in the
 *        setup menu). Set RP-side by cmdRunner because the m68k
 *        cannot write a handshake into the read-only cartridge area.
 *        Returns false for the GEMDRIVE-only [E]/[F]/countdown path.
 */
bool emul_isRunnerActive(void);

/**
 * @brief Most-recent Runner command the RP submitted (for
 *        GET /api/v1/runner). Returns RUNNER_LAST_NONE if no
 *        Runner command has been issued in this boot.
 */
runner_last_command_t emul_getRunnerLastCommand(void);

/**
 * @brief Timestamps (RP uptime ms) for the most-recent Runner
 *        command's submit / completion edges. RUNNER_RESET sets both
 *        to the same value (the m68k can't reply — the machine is
 *        rebooting).
 */
uint32_t emul_getRunnerLastStartedMs(void);
uint32_t emul_getRunnerLastFinishedMs(void);

/**
 * @brief Record a Runner command submission. Sets last_command and
 *        the started/finished timestamps to now_ms.
 */
void emul_recordRunnerCommand(runner_last_command_t cmd, uint32_t now_ms);

/**
 * @brief Schedule an automatic re-launch of Runner mode. After
 *        `runner reset` the ST cold-boots; once the m68k is back to
 *        its CA_INIT polling loop the main loop re-fires
 *        DISPLAY_COMMAND_START_RUNNER at `at_ms` so Runner mode is
 *        sticky across resets without operator interaction. Pass 0
 *        to cancel a pending relaunch.
 */
void emul_scheduleRunnerRelaunch(uint32_t at_ms);

#endif  // EMUL_H
