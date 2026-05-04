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

// Epic 06 / S5+S6 — Pexec(3) load + Pexec(4) exec split.
//
// Strict-refuse semantics: a `runner load` while
// emul_isRunnerLoadPending() is already true must return 409
// program_already_loaded. The user must `runner exec` (or bounce
// Runner mode) to consume the prior basepage before submitting
// another load. This keeps the m68k from holding orphan
// basepages the RP forgot about.

/** @brief Record a LOAD submission (Pexec(3)). Stores `path` for
 *         the status mirror, marks busy. The actual basepage is
 *         set by a later DONE_LOAD via emul_recordRunnerLoadDone. */
void emul_recordRunnerLoadSubmit(const char *path, uint32_t now_ms);

/** @brief Record a LOAD completion. `result > 0` is a basepage
 *         pointer (success — emul_isRunnerLoadPending becomes
 *         true). `result < 0` is a -GEMDOS errno (failure —
 *         emul_getRunnerLastLoadErrno reports it). Clears busy. */
void emul_recordRunnerLoadDone(int32_t result, uint32_t now_ms);

/** @brief Record an EXEC submission (Pexec(4)). Marks busy.
 *         Caller must have verified emul_isRunnerLoadPending(). */
void emul_recordRunnerExecSubmit(uint32_t now_ms);

/** @brief Record an EXEC completion. Stores exit_code and
 *         clears busy. pendingBasepage is preserved — `Pexec(4)`
 *         does NOT free the basepage, so the program stays
 *         loaded for re-exec; explicit unload (S7) is what
 *         releases the memory. */
void emul_recordRunnerExecDone(int32_t exit_code, uint32_t now_ms);

/** @brief Record an UNLOAD submission (GEMDOS Mfree on the
 *         loaded basepage). Marks busy. Caller must have
 *         verified `emul_isRunnerLoadPending()` first. */
void emul_recordRunnerUnloadSubmit(uint32_t now_ms);

/** @brief Record an UNLOAD completion. `result == 0` clears
 *         the pending basepage. `result < 0` (-GEMDOS errno)
 *         keeps the basepage flagged AND records the errno on
 *         the load-errno surface so a follow-up status call
 *         shows what failed. */
void emul_recordRunnerUnloadDone(int32_t result, uint32_t now_ms);

/** @brief True iff a program has been Pexec(3)-loaded and is
 *         waiting for an exec. Used to gate the load endpoint
 *         (strict refuse) and to gate the exec endpoint
 *         (no_program_loaded if false). */
bool emul_isRunnerLoadPending(void);

/** @brief Cached Pexec(3) basepage pointer. Returns 0 if no
 *         program is loaded (matches the "0 = NULL" convention
 *         the m68k Pexec API uses for an unset basepage). */
int32_t emul_getRunnerPendingBasepage(void);

/** @brief Returns true if the most recent load failed; *out
 *         gets the -GEMDOS errno. False if no load has been
 *         attempted yet OR the most recent load succeeded. */
bool emul_getRunnerLastLoadErrno(int32_t *out);

/**
 * @brief Record a CD submission. Stores the requested target path
 *        (used both as last_path and as the optimistic cwd while the
 *        m68k Dsetpath is in flight) and marks busy=true.
 */
void emul_recordRunnerCdSubmit(const char *path, uint32_t now_ms);

/**
 * @brief Record a CD completion (RUNNER_CMD_DONE_CD). Stores the
 *        GEMDOS errno; if errno != 0 reverts the cwd mirror to its
 *        previous value (Dsetpath leaves the m68k cwd unchanged on
 *        failure). Clears busy.
 */
void emul_recordRunnerCdDone(int32_t errnum, uint32_t now_ms);

/**
 * @brief Last-known Runner cwd. Empty string if no successful CD has
 *        landed in this boot.
 */
const char *emul_getRunnerCwd(void);

/**
 * @brief Last CD errno, if available. Returns true and writes the
 *        i32 into *out when a CD completion has been recorded; false
 *        (and leaves *out untouched) if not.
 */
bool emul_getRunnerLastCdErrno(int32_t *out);

/**
 * @brief Record a RUNNER_RES submission. Sets last_command=RES,
 *        timestamps, busy=true.
 */
void emul_recordRunnerResSubmit(uint32_t now_ms);

/**
 * @brief Record a RUNNER_RES completion (RUNNER_CMD_DONE_RES).
 *        Stores the m68k-side errno (0 = applied, -1 = monochrome
 *        ignored, -2 = bad rez). Clears busy.
 */
void emul_recordRunnerResDone(int32_t errnum, uint32_t now_ms);

/**
 * @brief Last RES errno, if available. Returns true and writes the
 *        i32 into *out when a RES completion has been recorded.
 */
bool emul_getRunnerLastResErrno(int32_t *out);

/**
 * @brief Record a MEMINFO submission (HTTP handler is about to fire
 *        RUNNER_CMD_MEMINFO and spin waiting for the m68k to reply).
 *        Sets last_command=MEMINFO, started_at_ms, busy=true, marks
 *        the snapshot stale.
 */
void emul_recordRunnerMeminfoSubmit(uint32_t now_ms);

/**
 * @brief Record a MEMINFO completion (RUNNER_CMD_DONE_MEMINFO).
 *        Stashes the snapshot, marks it fresh, clears busy,
 *        timestamps finished_at_ms.
 */
void emul_recordRunnerMeminfoDone(const runner_meminfo_t *snap,
                                  uint32_t now_ms);

/**
 * @brief Whether the last RUNNER_CMD_DONE_MEMINFO has arrived since
 *        the most-recent submit. Used by the synchronous HTTP
 *        handler to spin until the m68k replies.
 */
bool emul_isRunnerMeminfoReady(void);

/**
 * @brief Read out the last MEMINFO snapshot. Returns true and copies
 *        the struct into *out if a snapshot is available; false
 *        otherwise (no snapshot recorded yet, or session was reset).
 */
bool emul_getRunnerMeminfo(runner_meminfo_t *out);

/**
 * @brief Wipe session-transient Runner state (busy lock, cwd mirror,
 *        last cd-errno). Called when the m68k Runner reports it has
 *        (re)entered its poll loop via RUNNER_CMD_DONE_HELLO — covers
 *        physical/cold resets where the RP otherwise can't observe
 *        the m68k restart. last_command/last_path are preserved so
 *        the developer still has forensic visibility into what was
 *        running before the reset.
 */
void emul_resetRunnerSession(void);

/**
 * @brief Whether the m68k Runner has confirmed its Advanced Runner
 *        VBL hook (Epic 04) is installed at $70. Set via the HELLO
 *        message's payload byte every time runner_post_reloc runs;
 *        cleared on emul_resetRunnerSession() (which fires on the
 *        next HELLO too, so the value is effectively replaced
 *        atomically per session). Used by GET /api/v1/runner/adv.
 */
bool emul_isRunnerAdvancedInstalled(void);

/**
 * @brief Stash the Advanced-installed flag from the HELLO payload.
 *        Called by runner_command_cb on every HELLO arrival.
 */
void emul_recordRunnerAdvancedInstalled(bool installed);

/**
 * @brief Active hook vector ID reported by the m68k in the HELLO
 *        payload. One of RUNNER_HOOK_VECTOR_VBL,
 *        RUNNER_HOOK_VECTOR_ETV_TIMER, or RUNNER_HOOK_VECTOR_UNKNOWN.
 *        Used by GET /api/v1/runner/adv.
 */
uint8_t emul_getRunnerAdvHookVector(void);

/**
 * @brief Stash the hook-vector ID from the HELLO payload byte.
 */
void emul_recordRunnerAdvHookVector(uint8_t vector_id);

/**
 * @brief Per-chunk ack flag for the Advanced load streamer (S8). The
 *        chunk-done chandler sets it via emul_recordRunnerAdvLoadAck;
 *        the HTTP streamer spins on emul_isRunnerAdvLoadAcked +
 *        chandler_loop until the flag flips, then clears it via
 *        emul_clearRunnerAdvLoadAck before firing the next chunk.
 */
bool emul_isRunnerAdvLoadAcked(void);
void emul_recordRunnerAdvLoadAck(void);
void emul_clearRunnerAdvLoadAck(void);

/**
 * @brief The m68k just ran gemdrive_init (CMD_GEMDRIVE_HELLO arrived).
 *        That is the unambiguous "ST cold-booted" signal — true for
 *        first power-on, `runner reset`, the ST's physical reset
 *        button, and any other path that re-enters CA_INIT. If
 *        Runner mode was active before the reset, this kicks the
 *        relaunch ticker so the m68k jumps back into runner_entry
 *        without operator intervention. No-op otherwise.
 */
void emul_onGemdriveHello(void);

/**
 * @brief Schedule an automatic re-launch of Runner mode. After
 *        `runner reset` the ST cold-boots; once the m68k is back to
 *        its CA_INIT polling loop the main loop re-fires
 *        DISPLAY_COMMAND_START_RUNNER at `at_ms` so Runner mode is
 *        sticky across resets without operator interaction. Pass 0
 *        to cancel a pending relaunch.
 */
void emul_scheduleRunnerRelaunch(uint32_t at_ms);

/**
 * @brief Commit the firmware-mode transition (Epic 05 v2). Called
 *        from the menu code paths right before they hand control
 *        off — the user picked [U] / [E] / [F]. Idempotent and
 *        one-way: subsequent calls are no-ops.
 *
 *        The chandler ingest filter uses this flag to gate
 *        debug-byte capture: pre-firmware-mode emits get dropped
 *        at the handler so menu-mode noise doesn't pollute the
 *        diagnostic stream.
 */
void emul_enterFirmwareMode(void);

/**
 * @brief Whether emul_enterFirmwareMode() has been called this
 *        session.
 */
bool emul_isFirmwareMode(void);

#endif  // EMUL_H
