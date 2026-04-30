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
 * @brief Whether a Runner command is in flight (only EXECUTE for
 *        now). Cleared by emul_recordRunnerExecuteDone when the
 *        runner_command_cb chandler hook receives RUNNER_CMD_DONE.
 */
bool emul_isRunnerBusy(void);

/**
 * @brief Most-recent Runner command the RP submitted (for
 *        GET /api/v1/runner). Returns RUNNER_LAST_NONE if no
 *        Runner command has been issued in this boot.
 */
runner_last_command_t emul_getRunnerLastCommand(void);

/**
 * @brief Last submitted EXECUTE path (canonical normalised form).
 *        Empty string if last command wasn't an EXECUTE.
 */
const char *emul_getRunnerLastPath(void);

/**
 * @brief Last EXECUTE exit code, if available. Returns true and
 *        writes the i32 into *out when an exit code has been
 *        recorded by emul_recordRunnerExecuteDone(); false (and
 *        leaves *out untouched) if not.
 */
bool emul_getRunnerLastExitCode(int32_t *out);

/**
 * @brief Timestamps (RP uptime ms) for the most-recent Runner
 *        command's submit / completion edges. RUNNER_RESET sets both
 *        to the same value (the m68k can't reply — the machine is
 *        rebooting). EXECUTE leaves finished=0 until DONE arrives.
 */
uint32_t emul_getRunnerLastStartedMs(void);
uint32_t emul_getRunnerLastFinishedMs(void);

/**
 * @brief Record a fire-and-forget Runner command (RESET). Sets
 *        last_command and started/finished timestamps to now_ms,
 *        clears any prior exit code / path.
 */
void emul_recordRunnerCommand(runner_last_command_t cmd, uint32_t now_ms);

/**
 * @brief Record an EXECUTE submission. Sets last_command=EXECUTE,
 *        last_path, started_at_ms; flags busy=true; clears any
 *        prior exit code; finished_at_ms stays 0 until DONE.
 */
void emul_recordRunnerExecuteSubmit(const char *path, uint32_t now_ms);

/**
 * @brief Record an EXECUTE completion (RUNNER_CMD_DONE_EXECUTE on
 *        the chandler protocol). Stores exit_code, finished_at_ms,
 *        clears busy.
 */
void emul_recordRunnerExecuteDone(int32_t exit_code, uint32_t now_ms);

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
