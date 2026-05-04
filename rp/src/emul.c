/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "display_term.h"
#include "ff.h"
#include "gconfig.h"
#include "gemdrive.h"
#include "http_server.h"
#include "memfunc.h"
#include "runner.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"
#include "usbcdc.h"

// Main-loop tick (Epic 05 v2 / S9). Drives chandler_loop +
// usbcdc_drain at ~100 Hz so chandler can drain the 4096-sample
// commemul ring (~4 ms wrap at full m68k emit rate) with 10×
// headroom, and so CDC bytes appear on the workstation with
// minimal latency. Lower than this gets into busy-loop
// territory; higher and we re-introduce the visibility / drop
// problems that motivated the optimization stories. The full
// Core 1 worker plan (chandler + tud_task + usbcdc_drain on a
// dedicated core) is parked in the Epic 05 backlog — escalate
// there only if 100 Hz proves insufficient.
#define SLEEP_LOOP_MS 10

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Command handlers
static void cmdMenu(const char *arg);
static void cmdExit(const char *arg);
static void cmdFirmware(const char *arg);
static void cmdBooster(const char *arg);
static void cmdGemdriveFolder(const char *arg);
static void cmdGemdriveDrive(const char *arg);
static void cmdGemdriveRelocAddr(const char *arg);
static void cmdGemdriveMemtop(const char *arg);
static void cmdAdvHookVector(const char *arg);
static void cmdRunner(const char *arg);
static void cmdHiddenSettings(const char *arg);
static void cmdPrint(const char *arg);
static void cmdSave(const char *arg);
static void cmdErase(const char *arg);
static void cmdGet(const char *arg);
static void cmdPutInt(const char *arg);
static void cmdPutBool(const char *arg);
static void cmdPutString(const char *arg);

// Command table. Single-letter keys mirror md-drives-emulator. The hidden
// "?" entry exposes the raw settings commands (print/save/get/...).
static const Command commands[] = {
    {"m", cmdMenu},
    {"e", cmdExit},
    {"f", cmdFirmware},
    {"x", cmdBooster},
    {"o", cmdGemdriveFolder},
    {"d", cmdGemdriveDrive},
    {"r", cmdGemdriveRelocAddr},
    {"t", cmdGemdriveMemtop},
    {"v", cmdAdvHookVector},
    {"u", cmdRunner},
    {"?", cmdHiddenSettings},
    {"print", cmdPrint},
    {"save", cmdSave},
    {"erase", cmdErase},
    {"get", cmdGet},
    {"put_int", cmdPutInt},
    {"put_bool", cmdPutBool},
    {"put_str", cmdPutString},
};

// Number of commands in the table
static const size_t numCommands = sizeof(commands) / sizeof(commands[0]);

// Keep active loop or exit
static bool keepActive = true;
static bool menuScreenActive = false;

// Epic 06 / S4: cached "USB CDC attached" state from the last
// menu paint. -1 means "not yet rendered this menu session";
// any 0/1 transition triggers refreshUsbCdcLine() to overdraw
// the status line in place. Lives at module scope because both
// menu() (initial paint) and the main loop (live refresh) touch
// it. Fixed cursor row so the overdraw doesn't have to walk the
// terminal state.
#define MENU_USBCDC_ROW 15
static int g_menuLastUsbCdcAttached = -1;

// Runner state owned RP-side (the m68k can't write to the cartridge
// address space — read-only ROM emulation — so any handshake from
// the Runner has to live in RP RAM). cmdRunner flips active when
// the user picks [U] at boot; per-command handlers update
// last_command + timestamps; the runner_command_cb chandler hook
// records exit codes when the m68k Runner reports DONE_EXECUTE.
// GET /api/v1/runner surfaces this struct.
//
// Once active is set it stays set for the lifetime of the RP power
// cycle: a `runner reset` cold-reboots the ST and auto-relaunches
// straight back into Runner mode (no need to re-pick [U]) — that's
// the dev-iteration UX described in docs/epics/03-runner.md. The
// scheduled relaunch is driven from the main loop via
// runnerRelaunchAtMs.
static bool runnerActive = false;
static bool runnerBusy = false;
static runner_last_command_t runnerLastCommand = RUNNER_LAST_NONE;
static char runnerLastPath[RUNNER_PATH_LEN] = {0};
static bool runnerLastHasExitCode = false;
static int32_t runnerLastExitCode = 0;
static uint32_t runnerLastStartedMs = 0;
static uint32_t runnerLastFinishedMs = 0;
static uint32_t runnerRelaunchAtMs = 0;  // 0 = no pending relaunch
// Mirror of the m68k Runner's cwd. Updated optimistically on
// emul_recordRunnerCdSubmit (so a subsequent runner status during the
// brief in-flight window reflects the *intent*) and reverted in
// emul_recordRunnerCdDone if the m68k Dsetpath returned non-zero.
static char runnerCwd[RUNNER_CWD_LEN] = {0};
static char runnerCwdPrev[RUNNER_CWD_LEN] = {0};
static bool runnerLastHasCdErrno = false;
static int32_t runnerLastCdErrno = 0;
static bool runnerLastHasResErrno = false;
static int32_t runnerLastResErrno = 0;
static runner_meminfo_t runnerMeminfo = {0};
static bool runnerMeminfoHasSnapshot = false;
static bool runnerMeminfoPending = false;
// Epic 06 / S5+S6 — Pexec(3)/(4) load+exec split state. The
// m68k Runner's RUNNER_CMD_LOAD handler (Pexec mode 3) returns a
// basepage pointer in the DONE_LOAD payload; we cache it here so
// a subsequent RUNNER_CMD_EXEC fires Pexec(4) on the right
// program, and so the strict-refuse semantics on a second LOAD
// stay enforceable from the RP side without any m68k roundtrip.
// pendingBasepage is signed — the m68k payload format is "i32:
// >0 basepage ptr, <0 -GEMDOS errno". On error we store 0 so
// emul_isRunnerLoadPending() reads correctly; the errno survives
// in runnerLoadErrno for the status endpoint.
static int32_t runnerPendingBasepage = 0;
static bool runnerLoadHasErrno = false;
static int32_t runnerLoadErrno = 0;
// Epic 04 — Advanced Runner installation flag, set by the m68k's
// HELLO payload byte after runner_post_reloc installs its VBL hook.
static bool runnerAdvancedInstalled = false;
// S4 — active hook vector ID reported by the m68k in the HELLO
// payload. Default UNKNOWN until HELLO arrives.
static uint8_t runnerAdvHookVector = RUNNER_HOOK_VECTOR_UNKNOWN;
// S8 — Advanced load chunk-ack flag. Set by the chandler when a
// RUNNER_ADV_CMD_DONE_LOAD_CHUNK arrives, cleared by the streamer
// before firing each chunk.
static bool runnerAdvLoadAcked = false;

bool emul_isRunnerActive(void) { return runnerActive; }
bool emul_isRunnerBusy(void) { return runnerBusy; }

runner_last_command_t emul_getRunnerLastCommand(void) {
  return runnerLastCommand;
}

const char *emul_getRunnerLastPath(void) { return runnerLastPath; }

bool emul_getRunnerLastExitCode(int32_t *out) {
  if (runnerLastHasExitCode && out != NULL) {
    *out = runnerLastExitCode;
  }
  return runnerLastHasExitCode;
}

uint32_t emul_getRunnerLastStartedMs(void) { return runnerLastStartedMs; }
uint32_t emul_getRunnerLastFinishedMs(void) { return runnerLastFinishedMs; }

void emul_recordRunnerCommand(runner_last_command_t cmd, uint32_t now_ms) {
  runnerLastCommand = cmd;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = now_ms;
  runnerLastHasExitCode = false;
  runnerLastExitCode = 0;
  runnerLastPath[0] = '\0';
  if (cmd == RUNNER_LAST_RESET) {
    // The cold reset wipes the m68k TOS process cwd back to root —
    // clear our mirror so subsequent relative commands resolve from
    // the same baseline. Also forcibly clear the busy lock: the
    // program that was running owned the cartridge bus and never got
    // to send DONE; without this, status keeps reporting busy=true
    // forever and POST /run / /cd return 503.
    runnerBusy = false;
    runnerCwd[0] = '\0';
    runnerCwdPrev[0] = '\0';
    runnerLastHasCdErrno = false;
    runnerLastCdErrno = 0;
  }
}

void emul_recordRunnerExecuteSubmit(const char *path, uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_EXECUTE;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;  // not finished yet
  runnerLastHasExitCode = false;
  runnerLastExitCode = 0;
  if (path != NULL) {
    size_t n = strlen(path);
    if (n >= sizeof(runnerLastPath)) n = sizeof(runnerLastPath) - 1;
    memcpy(runnerLastPath, path, n);
    runnerLastPath[n] = '\0';
  } else {
    runnerLastPath[0] = '\0';
  }
  runnerBusy = true;
}

void emul_recordRunnerExecuteDone(int32_t exit_code, uint32_t now_ms) {
  runnerLastExitCode = exit_code;
  runnerLastHasExitCode = true;
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
}

// Epic 06 / S5+S6 — Pexec(3) load + Pexec(4) exec.
void emul_recordRunnerLoadSubmit(const char *path, uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_PEXEC_LOAD;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;
  runnerLastHasExitCode = false;
  runnerLastExitCode = 0;
  runnerLoadHasErrno = false;
  runnerLoadErrno = 0;
  if (path != NULL) {
    size_t n = strlen(path);
    if (n >= sizeof(runnerLastPath)) n = sizeof(runnerLastPath) - 1;
    memcpy(runnerLastPath, path, n);
    runnerLastPath[n] = '\0';
  } else {
    runnerLastPath[0] = '\0';
  }
  runnerBusy = true;
}

void emul_recordRunnerLoadDone(int32_t result, uint32_t now_ms) {
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
  if (result > 0) {
    runnerPendingBasepage = result;
    runnerLoadHasErrno = false;
    runnerLoadErrno = 0;
  } else {
    runnerPendingBasepage = 0;
    runnerLoadHasErrno = true;
    runnerLoadErrno = result;  // <0 = -GEMDOS errno; 0 = unexpected
  }
}

void emul_recordRunnerExecSubmit(uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_PEXEC_EXEC;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;
  runnerLastHasExitCode = false;
  runnerLastExitCode = 0;
  // exec doesn't operate on a path — keep the path mirror as it
  // was set by the prior load so a status query during the in-
  // flight window still shows which program is running.
  runnerBusy = true;
}

void emul_recordRunnerExecDone(int32_t exit_code, uint32_t now_ms) {
  runnerLastExitCode = exit_code;
  runnerLastHasExitCode = true;
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
  // Pexec(4) (PE_GO) does NOT free the basepage — it stays
  // allocated in m68k RAM and re-exec on the same basepage is
  // valid. The runner unload command (Epic 06 / S7) is what
  // explicitly Mfrees the memory and clears pendingBasepage.
}

// Epic 06 / S7 — runner unload (GEMDOS Mfree).
void emul_recordRunnerUnloadSubmit(uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_PEXEC_UNLOAD;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;
  runnerLastHasExitCode = false;
  runnerLastExitCode = 0;
  runnerLoadHasErrno = false;
  runnerLoadErrno = 0;
  runnerBusy = true;
}

void emul_recordRunnerUnloadDone(int32_t result, uint32_t now_ms) {
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
  if (result == 0) {
    // Mfree succeeded — basepage gone from m68k RAM, clear our
    // mirror so future load works and exec / unload report 409.
    runnerPendingBasepage = 0;
    runnerLoadHasErrno = false;
    runnerLoadErrno = 0;
  } else {
    // Mfree failed (e.g. -40 EIMBA). Keep pendingBasepage as-is
    // so the operator can see "this basepage still claims to be
    // loaded but a previous unload failed" and act accordingly.
    runnerLoadHasErrno = true;
    runnerLoadErrno = result;
  }
}

bool emul_isRunnerLoadPending(void) { return runnerPendingBasepage != 0; }
int32_t emul_getRunnerPendingBasepage(void) { return runnerPendingBasepage; }

bool emul_getRunnerLastLoadErrno(int32_t *out) {
  if (runnerLoadHasErrno && out != NULL) {
    *out = runnerLoadErrno;
  }
  return runnerLoadHasErrno;
}

void emul_recordRunnerCdSubmit(const char *path, uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_CD;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;
  runnerLastHasCdErrno = false;
  runnerLastCdErrno = 0;
  if (path != NULL) {
    size_t n = strlen(path);
    if (n >= sizeof(runnerLastPath)) n = sizeof(runnerLastPath) - 1;
    memcpy(runnerLastPath, path, n);
    runnerLastPath[n] = '\0';
    // Snapshot the prior cwd so we can revert if the m68k Dsetpath
    // fails — TOS leaves the active path unchanged on error.
    memcpy(runnerCwdPrev, runnerCwd, sizeof(runnerCwdPrev));
    n = strlen(path);
    if (n >= sizeof(runnerCwd)) n = sizeof(runnerCwd) - 1;
    memcpy(runnerCwd, path, n);
    runnerCwd[n] = '\0';
  } else {
    runnerLastPath[0] = '\0';
  }
  runnerBusy = true;
}

void emul_recordRunnerCdDone(int32_t errnum, uint32_t now_ms) {
  runnerLastCdErrno = errnum;
  runnerLastHasCdErrno = true;
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
  if (errnum != 0) {
    // Dsetpath failed — m68k cwd is unchanged. Revert our optimistic
    // mirror.
    memcpy(runnerCwd, runnerCwdPrev, sizeof(runnerCwd));
  }
}

const char *emul_getRunnerCwd(void) { return runnerCwd; }

bool emul_getRunnerLastCdErrno(int32_t *out) {
  if (runnerLastHasCdErrno && out != NULL) {
    *out = runnerLastCdErrno;
  }
  return runnerLastHasCdErrno;
}

void emul_recordRunnerResSubmit(uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_RES;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;
  runnerLastHasResErrno = false;
  runnerLastResErrno = 0;
  // RES doesn't operate on a path; clear the path mirror so the
  // status endpoint doesn't show a stale EXECUTE/CD path against an
  // RES last_command.
  runnerLastPath[0] = '\0';
  runnerBusy = true;
}

void emul_recordRunnerResDone(int32_t errnum, uint32_t now_ms) {
  runnerLastResErrno = errnum;
  runnerLastHasResErrno = true;
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
}

bool emul_getRunnerLastResErrno(int32_t *out) {
  if (runnerLastHasResErrno && out != NULL) {
    *out = runnerLastResErrno;
  }
  return runnerLastHasResErrno;
}

void emul_recordRunnerMeminfoSubmit(uint32_t now_ms) {
  runnerLastCommand = RUNNER_LAST_MEMINFO;
  runnerLastStartedMs = now_ms;
  runnerLastFinishedMs = 0;
  runnerLastPath[0] = '\0';
  runnerMeminfoPending = true;
  runnerBusy = true;
}

void emul_recordRunnerMeminfoDone(const runner_meminfo_t *snap,
                                  uint32_t now_ms) {
  if (snap != NULL) {
    runnerMeminfo = *snap;
    runnerMeminfoHasSnapshot = true;
  }
  runnerMeminfoPending = false;
  runnerLastFinishedMs = now_ms;
  runnerBusy = false;
}

bool emul_isRunnerMeminfoReady(void) { return !runnerMeminfoPending; }

bool emul_getRunnerMeminfo(runner_meminfo_t *out) {
  if (runnerMeminfoHasSnapshot && out != NULL) {
    *out = runnerMeminfo;
  }
  return runnerMeminfoHasSnapshot;
}

void emul_resetRunnerSession(void) {
  runnerBusy = false;
  runnerCwd[0] = '\0';
  runnerCwdPrev[0] = '\0';
  runnerLastHasCdErrno = false;
  runnerLastCdErrno = 0;
  runnerLastHasResErrno = false;
  runnerLastResErrno = 0;
  runnerMeminfoPending = false;
  runnerMeminfoHasSnapshot = false;
  runnerAdvancedInstalled = false;
  runnerAdvHookVector = RUNNER_HOOK_VECTOR_UNKNOWN;
}

bool emul_isRunnerAdvancedInstalled(void) {
  return runnerAdvancedInstalled;
}

void emul_recordRunnerAdvancedInstalled(bool installed) {
  runnerAdvancedInstalled = installed;
}

uint8_t emul_getRunnerAdvHookVector(void) { return runnerAdvHookVector; }

void emul_recordRunnerAdvHookVector(uint8_t vector_id) {
  runnerAdvHookVector = vector_id;
}

bool emul_isRunnerAdvLoadAcked(void) { return runnerAdvLoadAcked; }
void emul_recordRunnerAdvLoadAck(void) { runnerAdvLoadAcked = true; }
void emul_clearRunnerAdvLoadAck(void) { runnerAdvLoadAcked = false; }

void emul_onGemdriveHello(void) {
  if (!runnerActive) return;
  // The m68k just cold-booted (HELLO is sent from gemdrive_init,
  // which only runs at CA_INIT). If we were in Runner mode before,
  // schedule the relaunch ticker NOW so the print loop finds the
  // CMD_START_RUNNER sentinel within the next 500 ms. The ticker
  // re-fires until the Runner sends its own HELLO and cancels it.
  uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
  // 200 ms slack lets the m68k finish gemdrive_init + relocate +
  // reach the print loop before the first sentinel write lands.
  runnerRelaunchAtMs = now_ms + 200;
  // Also clear stuck busy/cwd here — physical reset never went
  // through handle_runner_reset, so emul_recordRunnerCommand never
  // ran and runnerBusy may still be true from the program that was
  // running when the user hit reset.
  runnerBusy = false;
  runnerCwd[0] = '\0';
  runnerCwdPrev[0] = '\0';
  runnerLastHasCdErrno = false;
  runnerLastCdErrno = 0;
  DPRINTF("emul: GEMDRIVE HELLO observed with runnerActive=true → "
          "scheduling relaunch\n");
}

void emul_scheduleRunnerRelaunch(uint32_t at_ms) {
  runnerRelaunchAtMs = at_ms;
}

// Epic 05 v2 — firmware-mode flag.
//
// One-way: only a hardware reset clears it. Used by the chandler
// ingest filter (chandler.c) to gate debug-byte capture: pre-menu
// emits get dropped at the handler so menu-mode noise doesn't
// pollute the diagnostic stream.
static bool firmwareModeActive = false;

void emul_enterFirmwareMode(void) {
  if (firmwareModeActive) {
    return;  // idempotent
  }
  firmwareModeActive = true;
  uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
  DPRINTF("emul: firmware mode committed at %lu ms\n",
          (unsigned long)now_ms);
}

bool emul_isFirmwareMode(void) {
  return firmwareModeActive;
}

// Boot countdown — auto-launches Runner mode on the Atari ST when it hits 0
// (Epic 06 / S2: switched from GEMDRIVE-only to Runner; the GEMDRIVE blob is
// still installed because Runner runs on top of it).
// Mirrors md-drives-emulator's behavior. Any key press halts it.
#define BOOT_COUNTDOWN_SECONDS 20
static int countdown = BOOT_COUNTDOWN_SECONDS;
static bool haltCountdown = false;
static absolute_time_t lastCountdownTick;

// Polling tick used as the network poll callback so command handling stays
// alive during multi-second WiFi operations.
static void __not_in_flash_func(emul_pollTick)(void) {
  chandler_loop();
  usbcdc_drain();
  term_loop();
}

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// --- Helpers ported verbatim from md-drives-emulator/rp/src/emul.c -----

// Returns the last n chars of str, prefixed with ".." if truncated.
// Caller frees. NULL on bad input.
static char *__not_in_flash_func(right)(const char *str, int n) {
  if (str == NULL || n < 0) return NULL;
  int len = (int)strlen(str);
  if (n == 0) return strdup("");
  if (n >= len) return strdup(str);
  const char *suffix = str + len - n;
  char *result = malloc(n + 3);
  if (!result) return NULL;
  strcpy(result, "..");
  strncpy(result + 2, suffix, n);
  result[n + 2] = '\0';
  return result;
}

// "C".."Z" only; identical predicate to source's GEMDRIVE drive validator.
static bool __not_in_flash_func(isValidDrive)(const char *drive) {
  if (drive == NULL || drive[0] == '\0') {
    return false;
  }
  char c = (char)toupper((unsigned char)drive[0]);
  return (c >= 'C' && c <= 'Z');
}

// VT52 absolute cursor move (ESC Y row col, both biased by 0x20).
static inline void vt52Cursor(uint8_t row, uint8_t col) {
  char vt52Seq[5];
  vt52Seq[0] = '\x1B';
  vt52Seq[1] = 'Y';
  vt52Seq[2] = (char)(32 + row);
  vt52Seq[3] = (char)(32 + col);
  vt52Seq[4] = '\0';
  term_printString(vt52Seq);
}

// Title with reverse-video (ESC p / ESC q) — matches the source's
// "two font sizes / dim+bright" feel by inverting the title bar.
static void showTitle(void) {
  term_printString(
      "\x1B"
      "E"
      "\x1Bp"
      "DevOps Microfirmware - " RELEASE_VERSION "\n\x1Bq");
}

static void __not_in_flash_func(showCounter)(int cdown);

// Bottom-of-OLED info strip rendered with the smaller squeezed font —
// this is the "second font size" referenced by the source. Inverts a 1px
// strip the height of one terminal row.
static void drawSetupInfoLine(const char *message) {
  u8g2_SetDrawColor(display_getU8g2Ref(), 1);
  u8g2_DrawBox(display_getU8g2Ref(), 0,
               DISPLAY_HEIGHT - DISPLAY_TERM_CHAR_HEIGHT, DISPLAY_WIDTH,
               DISPLAY_TERM_CHAR_HEIGHT);
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_squeezed_b7_tr);
  u8g2_SetDrawColor(display_getU8g2Ref(), 0);
  u8g2_DrawStr(display_getU8g2Ref(), 0, DISPLAY_HEIGHT - 1,
               (message != NULL) ? message : "");
  u8g2_SetDrawColor(display_getU8g2Ref(), 1);
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_amstrad_cpc_extended_8f);
}

static void refreshSetupInfoLine(void) {
  if (haltCountdown) {
    drawSetupInfoLine("Countdown stopped. Press [E], [U] or [X] to continue.");
  } else {
    showCounter(countdown);
  }
}

// Epic 06 / S8 — u8g2 menu polish (Tier 1). Thin horizontal
// dividers between the menu's config groups. Drawn in the gap
// row above each section header (term row 7 = y=56, row 10 =
// y=80, row 14 = y=112) so they don't overlap any character
// cell. One pixel tall, full-width edge-to-edge.
static void drawMenuDividers(void) {
  u8g2_t *ref = display_getU8g2Ref();
  u8g2_SetDrawColor(ref, 1);
  // Above Adv [V]ector (between row 6 GEMDRIVE last and row 8
  // Adv header).
  u8g2_DrawHLine(ref, 0, 60, DISPLAY_WIDTH);
  // Above API Endpoint (between row 9 and row 11).
  u8g2_DrawHLine(ref, 0, 84, DISPLAY_WIDTH);
  // Above USB CDC (between row 13 and row 15).
  u8g2_DrawHLine(ref, 0, 116, DISPLAY_WIDTH);
}

// Epic 06 / S8 — status icons via u8g2_font_open_iconic_embedded_1x_t.
// Right-aligned at the section-header rows. Codepoints from the
// 17-glyph open-iconic-embedded subset (verified via the u8g2
// upstream BDF):
//   0x42 cog          — Adv [V]ector (settings / hook handler)
//   0x4C hard-drive   — GEMDRIVE (file-storage emulation)
//   0x4D lightbulb    — USB CDC ("on / active connection";
//                       flash, pulse and bluetooth all proved
//                       unreadable at 8×8 in hardware testing —
//                       lightbulb's silhouette is the most
//                       legible at this resolution)
//   0x50 wifi         — API Endpoint
//
// Each icon's 8×8 cell is cleared first so a re-render flips
// state cleanly without a stale glyph hanging around. The
// drawIconCell helper takes y_top (the row's pixel-top); the
// glyph baseline is at y_top + 7 (font is 8 px tall).
#define MENU_ICON_X              (DISPLAY_WIDTH - 12)
#define MENU_ICON_GEMDRIVE_YTOP  16   // term row 2
#define MENU_ICON_ADV_YTOP       64   // term row 8
#define MENU_ICON_API_YTOP       88   // term row 11
#define MENU_ICON_USB_YTOP       120  // term row 15
#define MENU_ICON_GLYPH_COG          0x42
#define MENU_ICON_GLYPH_LIGHTBULB    0x4D
#define MENU_ICON_GLYPH_HARD_DRIVE   0x4C
#define MENU_ICON_GLYPH_WIFI         0x50

static void drawIconCell(uint16_t x, uint16_t y_top, uint8_t glyph,
                         bool visible) {
  u8g2_t *ref = display_getU8g2Ref();
  // Clear a 9×8 cell starting one pixel above y_top — some
  // glyphs in u8g2_font_open_iconic_embedded_1x_t (notably the
  // lightbulb) carry an ascender margin and their topmost
  // pixel row lands at y_top - 1, NOT y_top. An 8-tall erase
  // box would leave that row untouched and a hide→show
  // transition would expose a residual top edge of the prior
  // glyph. The four icon positions in this menu all have an
  // unused pixel row directly above (the hrules at y=60/84/116
  // sit further up still), so the +1 row of erase margin is
  // safe.
  u8g2_SetDrawColor(ref, 0);
  u8g2_DrawBox(ref, x, y_top - 1, 8, 9);
  if (visible) {
    u8g2_SetFont(ref, u8g2_font_open_iconic_embedded_1x_t);
    u8g2_SetDrawColor(ref, 1);
    u8g2_DrawGlyph(ref, x, y_top + 7, glyph);
    u8g2_SetFont(ref, u8g2_font_amstrad_cpc_extended_8f);
  }
  u8g2_SetDrawColor(ref, 1);
}

static void drawMenuStatusIcons(void) {
  // GEMDRIVE: hard-drive icon, always visible — section is
  // always populated and the icon serves as a static section
  // identifier rather than a live indicator.
  drawIconCell(MENU_ICON_X, MENU_ICON_GEMDRIVE_YTOP,
               MENU_ICON_GLYPH_HARD_DRIVE, true);
  // Adv [V]ector: cog icon, always visible — same rationale.
  drawIconCell(MENU_ICON_X, MENU_ICON_ADV_YTOP,
               MENU_ICON_GLYPH_COG, true);
  // Wi-Fi: drawn when DHCP has leased an IP. State doesn't
  // change after early boot in the steady-state menu, so this
  // doesn't need a live-refresh hook.
  ip_addr_t ip = network_getCurrentIp();
  drawIconCell(MENU_ICON_X, MENU_ICON_API_YTOP,
               MENU_ICON_GLYPH_WIFI, ip.addr != 0);
  // USB CDC: redrawn live by refreshUsbCdcLine when state flips.
  bool usb_attached = false;
  usbcdc_getStats(NULL, &usb_attached);
  drawIconCell(MENU_ICON_X, MENU_ICON_USB_YTOP,
               MENU_ICON_GLYPH_LIGHTBULB, usb_attached);
}

// Epic 06 / S8 — animated countdown progress bar. Replaces the
// "Boot will continue in N seconds..." text strip with a filled
// box that shrinks from full-width to zero as the countdown
// elapses. Text is rendered once over the filled (white) half
// in black and once over the empty (black) half in white, with
// u8g2 clip windows masking the two passes — that way each
// character cell is painted in its proper background-inverted
// colour. (XOR drawing was the obvious one-pass approach but
// u8g2's default font mode XORs the full glyph bounding box,
// so on a white background the letters come out as solid black
// blocks. Two clipped passes side-step that.)
static void drawCountdownBar(int sec_left, int sec_total) {
  if (sec_total <= 0) sec_total = 1;
  if (sec_left < 0) sec_left = 0;
  if (sec_left > sec_total) sec_left = sec_total;
  uint16_t y = DISPLAY_HEIGHT - DISPLAY_TERM_CHAR_HEIGHT;
  uint16_t h = DISPLAY_TERM_CHAR_HEIGHT;
  u8g2_t *ref = display_getU8g2Ref();

  // Clear strip (right / empty side stays black).
  u8g2_SetDrawColor(ref, 0);
  u8g2_DrawBox(ref, 0, y, DISPLAY_WIDTH, h);

  // Fill the proportional left (elapsed) portion in white.
  uint16_t bar_w = (uint16_t)((DISPLAY_WIDTH * sec_left) / sec_total);
  if (bar_w > 0) {
    u8g2_SetDrawColor(ref, 1);
    u8g2_DrawBox(ref, 0, y, bar_w, h);
  }

  // Build the status message once; draw it twice with different
  // clip windows + colours so each half of the bar reads
  // correctly. SetClipWindow takes (x0, y0, x1, y1) — y1/x1 are
  // exclusive upper bounds.
  char msg[40];
  int n = snprintf(msg, sizeof(msg),
                   "Booting in %d s — any key halts", sec_left);
  if (n < 0) msg[0] = '\0';
  u8g2_SetFont(ref, u8g2_font_squeezed_b7_tr);

  if (bar_w > 0) {
    // Filled (white) half: text in black (color 0).
    u8g2_SetClipWindow(ref, 0, y, bar_w, y + h);
    u8g2_SetDrawColor(ref, 0);
    u8g2_DrawStr(ref, 2, DISPLAY_HEIGHT - 1, msg);
  }
  if (bar_w < DISPLAY_WIDTH) {
    // Empty (black) half: text in white (color 1).
    u8g2_SetClipWindow(ref, bar_w, y, DISPLAY_WIDTH, y + h);
    u8g2_SetDrawColor(ref, 1);
    u8g2_DrawStr(ref, 2, DISPLAY_HEIGHT - 1, msg);
  }

  // Reset clip + state so subsequent draws are unaffected.
  u8g2_SetMaxClipWindow(ref);
  u8g2_SetDrawColor(ref, 1);
  u8g2_SetFont(ref, u8g2_font_amstrad_cpc_extended_8f);
}

// Epic 06 / S4. Polled from the main loop while the menu screen
// is up; on a state transition (host plugged in / unplugged) it
// overdraws just the USB CDC status line at MENU_USBCDC_ROW + 1
// rather than rebuilding the whole menu. Both display strings
// are 12 chars long so the overdraw fully covers the previous
// value with no stale tail bytes. Cheap when there's no change
// (one bool comparison).
static void refreshUsbCdcLine(void) {
  if (!menuScreenActive) return;
  bool attached = false;
  usbcdc_getStats(NULL, &attached);
  if ((int)attached == g_menuLastUsbCdcAttached) return;
  g_menuLastUsbCdcAttached = (int)attached;
  vt52Cursor(MENU_USBCDC_ROW + 1, 0);
  term_printString("  Status      : ");
  term_printString(attached ? "connected   " : "disconnected");
  // Epic 06 / S8 — flip the lightbulb ("active") icon at the
  // USB CDC header in lock-step with the status text.
  drawIconCell(MENU_ICON_X, MENU_ICON_USB_YTOP,
               MENU_ICON_GLYPH_LIGHTBULB, attached);
  // Restore the cursor home position (right after the
  // "Select an option: " prompt on the bottom row) so the
  // overdraw doesn't leave it stranded over the USB CDC line.
  // "Select an option: " is 18 chars, so col 18 is the spot
  // the user expects to type into.
  vt52Cursor(TERM_SCREEN_SIZE_Y - 1, 18);
  display_refresh();
}

// --- Directory navigation pager (ported from md-drives-emulator) ------

#define NAV_LINES_PER_PAGE 16
#define NAV_LINES_PER_PAGE_OFFSET 4
enum navStatus {
  NAV_DIR_ERROR = -1,
  NAV_DIR_FIRST_TIME_OK = 0,
  NAV_DIR_NEXT_TIME_OK = 1,
  NAV_DIR_SELECTED = 2,
  NAV_DIR_CANCEL = 3,
};

typedef struct {
  uint16_t count;
  uint16_t selected;
  uint16_t page;
  char entries[MAX_ENTRIES_DIR][MAX_FILENAME_LENGTH + 1];
  char folderPath[MAX_FILENAME_LENGTH + 1];
  char topDir[MAX_FILENAME_LENGTH + 1];
} DirNavigation;

static DirNavigation navStateStorage;
static DirNavigation *navState = &navStateStorage;

// Filter: directories only — we list folders, never files. Hidden dotfiles
// are skipped. Mirrors the spirit of source's floppiesFilter for our case.
static bool __not_in_flash_func(foldersOnlyFilter)(const char *name,
                                                   BYTE attr) {
  if (name[0] == '.') {
    return false;
  }
  return (attr & AM_DIR) != 0;
}

// Remove last path component (".." navigation).
static void __not_in_flash_func(pathUp)(void) {
  char temp[MAX_FILENAME_LENGTH + 1];
  char *segments[MAX_ENTRIES_DIR];
  int sp = 0;

  strncpy(temp, navState->folderPath, sizeof(temp));
  temp[sizeof(temp) - 1] = '\0';

  char *token = strtok(temp, "/");
  while (token) {
    if (strcmp(token, "..") == 0) {
      if (sp > 0) sp--;
    } else if (strcmp(token, "") != 0) {
      segments[sp++] = token;
    }
    token = strtok(NULL, "/");
  }

  if (sp == 0) {
    strcpy(navState->folderPath, "/");
  } else {
    char newPath[MAX_FILENAME_LENGTH + 1] = "";
    for (int i = 0; i < sp; ++i) {
      strlcat(newPath, "/", sizeof(newPath));
      strlcat(newPath, segments[i], sizeof(newPath));
    }
    if (newPath[0] != '/') {
      char tmp[MAX_FILENAME_LENGTH + 1];
      snprintf(tmp, sizeof(tmp), "/%s", newPath);
      strncpy(newPath, tmp, sizeof(newPath));
    }
    strncpy(navState->folderPath, newPath, sizeof(navState->folderPath));
    navState->folderPath[sizeof(navState->folderPath) - 1] = '\0';
  }
}

static void drawPage(uint16_t top_offset) {
  uint16_t start = navState->page * NAV_LINES_PER_PAGE;
  uint16_t end = start + NAV_LINES_PER_PAGE;
  if (end > navState->count) {
    end = navState->count;
  }

  for (uint16_t i = start; i < end; i++) {
    uint8_t row = i - start;
    vt52Cursor(row + top_offset, 0);
    term_printString(i == navState->selected ? ">" : " ");
    char buffer[TERM_SCREEN_SIZE_X + 1];
    snprintf(buffer, sizeof(buffer), " %-*s", TERM_SCREEN_SIZE_X - 2,
             navState->entries[i]);
    term_printString(buffer);
  }

  vt52Cursor(TERM_SCREEN_SIZE_Y - 3, 0);
  char infoBuffer[128];
  int totalPages =
      (navState->count + NAV_LINES_PER_PAGE - 1) / NAV_LINES_PER_PAGE;
  sprintf(infoBuffer, "Page %d/%d\n", navState->page + 1, totalPages);
  term_printString(infoBuffer);
  term_printString("Use cursor keys and RETURN to navigate.\n");
  term_printString("SPACE to confirm selection. ESC to exit");
}

static enum navStatus __not_in_flash_func(navigate_directory)(
    bool first_time, bool dirs_only, char key, EntryFilterFn filter_fn,
    char top_folder[MAX_FILENAME_LENGTH + 1]) {
  enum navStatus status = NAV_DIR_ERROR;
  if (first_time) {
    DPRINTF("First time loading directory.\n");
    navState->count = 0;
    navState->selected = 0;
    navState->page = 0;
    if (top_folder != NULL) {
      strncpy(navState->topDir, top_folder, sizeof(navState->topDir));
      navState->topDir[sizeof(navState->topDir) - 1] = '\0';
    } else {
      strncpy(navState->topDir, "/", sizeof(navState->topDir));
      navState->topDir[sizeof(navState->topDir) - 1] = '\0';
    }
    memset(navState->entries, 0, sizeof(navState->entries));
    FRESULT result = sdcard_loadDirectory(
        (strlen(navState->folderPath) == 0) ? "/" : navState->folderPath,
        navState->entries, &navState->count, &navState->selected,
        &navState->page, dirs_only, filter_fn, navState->topDir);
    if (result != FR_OK) {
      term_printString("Error loading directory.\n");
    } else {
      status = NAV_DIR_FIRST_TIME_OK;
    }
  } else {
    DPRINTF("Next times loading directory.\n");
    status = NAV_DIR_NEXT_TIME_OK;
    int totalPages =
        (navState->count + NAV_LINES_PER_PAGE - 1) / NAV_LINES_PER_PAGE;
    switch (key) {
      case TERM_KEYBOARD_KEY_UP:
        if (navState->selected > 0) {
          navState->selected--;
        }
        break;
      case TERM_KEYBOARD_KEY_DOWN:
        if ((navState->selected < navState->count - 1) &&
            (navState->selected % NAV_LINES_PER_PAGE <
             NAV_LINES_PER_PAGE - 1)) {
          navState->selected++;
        }
        break;
      case TERM_KEYBOARD_KEY_LEFT:
        if (navState->page > 0) {
          navState->page--;
          navState->selected = navState->page * NAV_LINES_PER_PAGE;
        }
        break;
      case TERM_KEYBOARD_KEY_RIGHT:
        if (navState->page < (totalPages - 1)) {
          navState->page++;
          navState->selected = navState->page * NAV_LINES_PER_PAGE;
        }
        break;
      case '\r':
      case '\n': {
        if ((navState->entries[navState->selected]
                              [strlen(navState->entries[navState->selected]) -
                               1] == '/') ||
            (strcmp(navState->entries[navState->selected], "..") == 0)) {
          navState->count = 0;
          navState->page = 0;
          char newFolderPath[MAX_FILENAME_LENGTH + 1];
          size_t len = strlen(navState->folderPath);
          if (len > 0 && navState->folderPath[len - 1] != '/') {
            snprintf(newFolderPath, sizeof(newFolderPath), "%s/%s",
                     navState->folderPath,
                     navState->entries[navState->selected]);
          } else {
            snprintf(newFolderPath, sizeof(newFolderPath), "%s%s",
                     navState->folderPath,
                     navState->entries[navState->selected]);
          }
          strncpy(navState->folderPath, newFolderPath, sizeof(newFolderPath));
          pathUp();
          memset(navState->entries, 0, sizeof(navState->entries));
          navState->selected = 0;
          FRESULT result = sdcard_loadDirectory(
              navState->folderPath, navState->entries, &navState->count,
              &navState->selected, &navState->page, dirs_only, filter_fn,
              navState->topDir);
          if (result != FR_OK) {
            term_printString("Error loading directory.\n");
            status = NAV_DIR_ERROR;
          }
        }
        break;
      }
      case ' ': {
        status = NAV_DIR_SELECTED;
        break;
      }
      default:
        if (key >= 'a' && key <= 'z') {
          key = key - 'a' + 'A';
        }
        if (key >= 'A' && key <= 'Z') {
          status = key;
        }
        break;
    }
  }
  return status;
}

// Builds the menu — single GEMDRIVE block + bottom navigation strip.
// Layout follows the source's menu(): vt52Cursor positions, the F[o]lder/
// [D]rive labels, and the bottom "[E]xit / [X] Return to Booster" line.
static void __not_in_flash_func(menu)(void) {
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  menuScreenActive = true;

  showTitle();

  // Folder + drive read straight from aconfig, like source/md-drives.
  vt52Cursor(2, 0);
  term_printString("GEMDRIVE\n");

  SettingsConfigEntry *gemDriveFolder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
  const char *folderValue =
      (gemDriveFolder != NULL && gemDriveFolder->value[0] != '\0')
          ? gemDriveFolder->value
          : "/devops";
  char *folderTail = right(folderValue, 24);
  term_printString("  F[o]lder    : ");
  term_printString((folderTail != NULL) ? folderTail : folderValue);
  if (folderTail != NULL) free(folderTail);

  SettingsConfigEntry *gemDriveDrive =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_DRIVE);
  const char *driveValue =
      (gemDriveDrive != NULL && gemDriveDrive->value[0] != '\0')
          ? gemDriveDrive->value
          : "C";
  term_printString("\n  [D]rive     : ");
  term_printString(driveValue);
  term_printString(":");

  // Reloc / memtop overrides — 0 means "auto" (default = screen_base - 8 KB).
  SettingsConfigEntry *relocEntry = settings_find_entry(
      aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_RELOC_ADDR);
  int relocAddr = (relocEntry != NULL) ? atoi(relocEntry->value) : 0;
  char relocLine[48];
  if (relocAddr == 0) {
    snprintf(relocLine, sizeof(relocLine),
             "\n  [R]eloc addr: auto (screen-8KB)");
  } else {
    snprintf(relocLine, sizeof(relocLine), "\n  [R]eloc addr: 0x%06X",
             (unsigned)relocAddr);
  }
  term_printString(relocLine);

  SettingsConfigEntry *memtopEntry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_DEVOPS_MEMTOP);
  int memtop = (memtopEntry != NULL) ? atoi(memtopEntry->value) : 0;
  char memtopLine[48];
  if (memtop == 0) {
    snprintf(memtopLine, sizeof(memtopLine),
             "\n  Mem[t]op    : auto (matches reloc)");
  } else {
    snprintf(memtopLine, sizeof(memtopLine), "\n  Mem[t]op    : 0x%06X",
             (unsigned)memtop);
  }
  term_printString(memtopLine);

  term_printString("\n\n");

  // Advanced Runner hook vector — Epic 04 / S4. Toggleable via [V].
  // Epic 06 / S3: lifted out of the GEMDRIVE block onto its own
  // top-level row so the menu reads as three distinct config groups
  // (GEMDRIVE / Adv Vector / API Endpoint) rather than a wall of
  // GEMDRIVE-prefixed sub-items.
  SettingsConfigEntry *advHookEntry = settings_find_entry(
      aconfig_getContext(), ACONFIG_PARAM_ADV_HOOK_VECTOR);
  const char *advHookValue =
      (advHookEntry != NULL && advHookEntry->value[0] != '\0')
          ? advHookEntry->value
          : "etv_timer";
  const char *advHookDisplay = (strcmp(advHookValue, "vbl") == 0)
                                   ? "vbl ($70)"
                                   : "etv_timer ($400)";
  term_printString("Adv [V]ector\n");
  term_printString("  Hook        : ");
  term_printString(advHookDisplay);
  term_printString("\n\n");

  // Remote HTTP API endpoint (Epic 02). Show the leased IP and the
  // mDNS hostname so the user can hit it from a PC without guessing.
  ip_addr_t apiIp = network_getCurrentIp();
  SettingsConfigEntry *hostnameEntry =
      settings_find_entry(gconfig_getContext(), PARAM_HOSTNAME);
  const char *hostname =
      (hostnameEntry != NULL && hostnameEntry->value[0] != '\0')
          ? hostnameEntry->value
          : "sidecart";
  char urlLine[80];
  char ipLine[80];
  snprintf(urlLine, sizeof(urlLine), "  URL         : http://%s.local/",
           hostname);
  if (apiIp.addr != 0) {
    snprintf(ipLine, sizeof(ipLine), "  IP address  : %s",
             ipaddr_ntoa(&apiIp));
  } else {
    snprintf(ipLine, sizeof(ipLine), "  IP address  : (no IP)");
  }
  term_printString("API Endpoint\n");
  term_printString(urlLine);
  term_printString("\n");
  term_printString(ipLine);

  // Epic 06 / S4: USB CDC connection status. Painted at a fixed
  // row so refreshUsbCdcLine() (called from the main loop) can
  // overdraw the status text in place when the host attaches /
  // detaches without rebuilding the rest of the menu.
  bool usbAttached = false;
  usbcdc_getStats(NULL, &usbAttached);
  g_menuLastUsbCdcAttached = (int)usbAttached;
  vt52Cursor(MENU_USBCDC_ROW, 0);
  term_printString("USB CDC (Debug serial)");
  vt52Cursor(MENU_USBCDC_ROW + 1, 0);
  term_printString("  Status      : ");
  term_printString(usbAttached ? "connected   " : "disconnected");

  vt52Cursor(TERM_SCREEN_SIZE_Y - 2, 0);
  term_printString("[E]xit (launch)  r[U]nner  [X] Booster");

  vt52Cursor(TERM_SCREEN_SIZE_Y - 1, 0);
  term_printString("Select an option: ");

  // Epic 06 / S8 — overlay u8g2 dividers + icons AFTER all term
  // writes are done so the term renderer doesn't clobber them.
  // Frames around config groups are NOT drawn here because
  // vertical borders would slice through character columns and
  // corrupt the text — see Tier 2 backlog if framed sections
  // become a requirement.
  drawMenuDividers();
  drawMenuStatusIcons();

  refreshSetupInfoLine();
}

static void __not_in_flash_func(showCounter)(int cdown) {
  if (cdown > 0) {
    // Epic 06 / S8 — animated progress bar on the bottom strip
    // instead of plain text. Visual + textual at once.
    drawCountdownBar(cdown, BOOT_COUNTDOWN_SECONDS);
  } else {
    showTitle();
    drawSetupInfoLine("Booting... Please wait...");
  }
}

// --- Command handlers (single-key dispatch) ----------------------------

void cmdMenu(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menu();
}

// [E]xit doubles as the firmware-launch shortcut now: per the user's
// directive, leaving the menu drops straight into GEMDRIVE on the Atari ST
// (same path as [F]).
void cmdExit(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  showTitle();
  term_printString("\n\n");
  term_printString("Launching DevOps on the Atari ST...\n");
  // Epic 05 v2 — commit firmware mode (debug-byte filter starts
  // accepting captures from this point on).
  emul_enterFirmwareMode();
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
}

void cmdFirmware(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  term_printString("Launching DevOps on the Atari ST...\n");
  // Epic 05 v2 — commit firmware mode. This site also covers the
  // boot-countdown auto-launch path, which calls cmdFirmware when
  // the timer hits zero.
  emul_enterFirmwareMode();
  // CMD_START is the cartridge sentinel that GEMDRIVE polls during
  // pre_auto; receiving it makes the m68k jump into the GEMDRIVE blob.
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
}

// [U] launches Runner mode (Epic 03). Same shape as cmdFirmware but
// sends DISPLAY_COMMAND_START_RUNNER, which the m68k's check_commands
// dispatch maps to runner_function (jmp RUNNER_BLOB). GEMDRIVE has
// already been installed by pre_auto's gemdrive_init — the Runner
// runs alongside it, in foreground.
void cmdRunner(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  showTitle();
  term_printString("\n\n");
  term_printString("Launching DevOps Runner on the Atari ST...\n");
  // Flip the RP-side active flag so GET /api/v1/runner answers
  // "active": true even though the m68k Runner can't write a
  // handshake into the read-only cartridge area.
  runnerActive = true;
  // Epic 05 v2 — commit firmware mode (enables the debug-byte
  // capture filter for the rest of the session).
  emul_enterFirmwareMode();
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START_RUNNER);
}

void cmdBooster(const char *arg) {
  (void)arg;
  showTitle();
  term_printString("\n\n");
  term_printString("Launching Booster app...\n");
  term_printString("The computer will boot shortly...\n\n");
  term_printString("If it doesn't boot, power it on and off.\n");
  haltCountdown = true;
  menuScreenActive = false;
  resetDeviceAtBoot = false;  // Jump to the booster app
  keepActive = false;
}

// Folder picker — directly ported from md-drives-emulator's
// cmdGemdriveFolder. First press of [O] paints the SD card directory at
// the currently configured folder; subsequent keystrokes (arrows / RETURN
// / SPACE) come back through TERM_COMMAND_LEVEL_COMMAND_SINGLE_KEY_REENTRY
// and drive navigate_directory.
void __not_in_flash_func(cmdGemdriveFolder)(const char *arg) {
  haltCountdown = true;
  enum navStatus status = NAV_DIR_ERROR;
  switch (term_getCommandLevel()) {
    case TERM_COMMAND_LEVEL_SINGLE_KEY: {
      DPRINTF("Folder picker entering reentry mode.\n");
      SettingsConfigEntry *gemDriveFolder = settings_find_entry(
          aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
      const char *seed =
          (gemDriveFolder != NULL && gemDriveFolder->value[0] != '\0')
              ? gemDriveFolder->value
              : "/";
      strncpy(navState->folderPath, seed, sizeof(navState->folderPath));
      navState->folderPath[sizeof(navState->folderPath) - 1] = '\0';
      term_setCommandLevel(TERM_COMMAND_LEVEL_COMMAND_SINGLE_KEY_REENTRY);
      status = navigate_directory(true, true, '\0', foldersOnlyFilter, NULL);
      break;
    }
    case TERM_COMMAND_LEVEL_COMMAND_SINGLE_KEY_REENTRY: {
      DPRINTF("Folder picker key: %d\n", arg[0]);
      char key = arg[0];
      // ESC cancels the picker and returns to the menu.
      if (key == 27) {
        term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
        menu();
        return;
      }
      status = navigate_directory(false, true, key, foldersOnlyFilter, NULL);
      break;
    }
    default:
      break;
  }
  switch (status) {
    case NAV_DIR_FIRST_TIME_OK:
    case NAV_DIR_NEXT_TIME_OK: {
      showTitle();
      term_printString("\nFolder in the micro SD card: ");
      term_printString(navState->folderPath);
      drawPage(NAV_LINES_PER_PAGE_OFFSET);
      break;
    }
    case NAV_DIR_SELECTED: {
      settings_put_string(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER,
                          navState->folderPath);
      settings_save(aconfig_getContext(), true);
      term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
      menu();
      break;
    }
    default:
      break;
  }
}

// Drive picker — verbatim adaptation of source's cmdGemdriveDrive.
void cmdGemdriveDrive(const char *arg) {
  haltCountdown = true;
  if (term_getCommandLevel() == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    showTitle();
    term_printString("\n\n");
    term_printString("Enter the drive (C to Z):\n\n> ");
    term_setCommandLevel(TERM_COMMAND_LEVEL_DATA_INPUT);
    return;
  }
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  const char *input = (arg != NULL) ? arg : "";
  if (!isValidDrive(input)) {
    term_printString("Invalid drive. Press SPACE to continue...\n");
    return;
  }
  char driveBuffer[2] = {(char)toupper((unsigned char)input[0]), '\0'};
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_DRIVE,
                      driveBuffer);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Reloc-address picker. Same single-key → data-input pattern as drive.
// Accepts hex (0x...) or decimal. 0 / "auto" / empty restores the default.
void cmdGemdriveRelocAddr(const char *arg) {
  haltCountdown = true;
  if (term_getCommandLevel() == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    showTitle();
    term_printString("\n\n");
    term_printString("Enter the relocation address (hex 0x...):\n");
    term_printString("Use 0 or empty for auto (screen-8KB).\n\n> ");
    term_setCommandLevel(TERM_COMMAND_LEVEL_DATA_INPUT);
    return;
  }
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  const char *input = (arg != NULL) ? arg : "";
  // Trim leading spaces.
  while (*input == ' ' || *input == '\t') input++;
  long value = 0;
  if ((input[0] == '\0') || (strcasecmp(input, "auto") == 0)) {
    value = 0;
  } else {
    char *end = NULL;
    int base =
        (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) ? 16 : 10;
    value = strtol(input, &end, base);
    if ((end == input) || (value < 0)) {
      term_printString("Invalid address. Press SPACE to continue...\n");
      return;
    }
  }
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_RELOC_ADDR,
                       (int)value);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Memtop picker. Same shape as the reloc one. 0 mirrors reloc by default.
void cmdGemdriveMemtop(const char *arg) {
  haltCountdown = true;
  if (term_getCommandLevel() == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    showTitle();
    term_printString("\n\n");
    term_printString("Enter the _memtop value (hex 0x...):\n");
    term_printString("Use 0 or empty to follow the reloc address.\n\n> ");
    term_setCommandLevel(TERM_COMMAND_LEVEL_DATA_INPUT);
    return;
  }
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  const char *input = (arg != NULL) ? arg : "";
  while (*input == ' ' || *input == '\t') input++;
  long value = 0;
  if ((input[0] == '\0') || (strcasecmp(input, "auto") == 0)) {
    value = 0;
  } else {
    char *end = NULL;
    int base =
        (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) ? 16 : 10;
    value = strtol(input, &end, base);
    if ((end == input) || (value < 0)) {
      term_printString("Invalid memtop. Press SPACE to continue...\n");
      return;
    }
  }
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_DEVOPS_MEMTOP,
                       (int)value);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Advanced Runner hook-vector toggle (Epic 04 / S4). Single keypress
// cycles between "vbl" and "etv_timer" — no data-input flow because
// only two values are valid. Effective on the next ST cold reset
// (the m68k reads slot 16 once at runner_post_reloc); the menu
// reflects the *new* setting immediately so the user knows the
// toggle worked.
void cmdAdvHookVector(const char *arg) {
  (void)arg;
  haltCountdown = true;
  SettingsConfigEntry *entry = settings_find_entry(
      aconfig_getContext(), ACONFIG_PARAM_ADV_HOOK_VECTOR);
  const char *current = (entry != NULL && entry->value[0] != '\0')
                            ? entry->value
                            : "etv_timer";
  const char *next =
      (strcmp(current, "vbl") == 0) ? "etv_timer" : "vbl";
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ADV_HOOK_VECTOR,
                      next);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Hidden command list — switches the term to line-based COMMAND_INPUT so
// the user can type 'print', 'save', 'get key', etc.
void cmdHiddenSettings(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  showTitle();
  term_printString(
      "\n\n"
      "Available settings commands:\n"
      "  print   - Show settings\n"
      "  save    - Save settings\n"
      "  erase   - Erase settings\n"
      "  get     - Get setting (requires key)\n"
      "  put_int - Set integer (key and value)\n"
      "  put_bool- Set boolean (key and value)\n"
      "  put_str - Set string (key and value)\n\n"
      "Enter command. Type 'm' to return > ");
  term_setCommandLevel(TERM_COMMAND_LEVEL_COMMAND_INPUT);
}

void cmdPrint(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPrint(arg);
}

void cmdSave(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdSave(arg);
  // Cartridge-side state (GEMDRIVE relocation, _memtop patch, etc.) is
  // applied by gemdrive_init at CA_INIT time, so a saved aconfig change
  // is invisible until the m68k re-runs CA_INIT. Issue an automatic ST
  // reset so the change takes effect immediately.
  term_printString("Resetting Atari ST to apply changes...\n");
  display_refresh();
  sleep_ms(300);
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
}

void cmdErase(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdErase(arg);
}

void cmdGet(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdGet(arg);
}

void cmdPutInt(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPutInt(arg);
}

void cmdPutBool(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPutBool(arg);
}

void cmdPutString(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPutString(arg);
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString("Configuring network... please wait...\n");

  display_refresh();
}

void failure(const char *message) {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString(message);

  display_refresh();
}

static void init(void) {
  // Set the command table
  term_setCommands(commands, numCommands);

  // Clear the screen
  term_clearScreen();

  // Display the menu
  menu();

  // Example 1: Move the cursor up one line.
  // VT52 sequence: ESC A (moves cursor up)
  // The escape sequence "\x1BA" will move the cursor up one line.
  // term_printString("\x1B" "A");
  // After moving up, print text that overwrites part of the previous line.
  // term_printString("Line 2 (modified by ESC A)\n");

  // Example 2: Move the cursor right one character.
  // VT52 sequence: ESC C (moves cursor right)
  // term_printString("\x1B" "C");
  // term_printString(" <-- Moved right with ESC C\n");

  // Example 3: Direct cursor addressing.
  // VT52 direct addressing uses ESC Y <row> <col>, where:
  //   row_char = row + 0x20, col_char = col + 0x20.
  // For instance, to move the cursor to row 0, column 10:
  //   row: 0 -> 0x20 (' ')
  //   col: 10 -> 0x20 + 10 = 0x2A ('*')
  // term_printString("\x1B" "Y" "\x20" "\x2A");
  // term_printString("Text at row 0, column 10 via ESC Y\n");

  // term_printString("\x1B" "Y" "\x2A" "\x20");

  display_refresh();
}

void emul_start() {
  // Bring up the USB CDC sink for the debugcap ring (Epic 05 v2 / S5).
  // Idempotent stdio_init_all + detaches stdio from CDC so DPRINTF
  // stays UART-only and the CDC interface is the dedicated raw-byte
  // channel for captured debug bytes.
  usbcdc_init();

  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(term_command_cb);

  // Register GEMDRIVE before the cartridge boots — the m68k issues
  // CMD_GEMDRIVE_HELLO from CA_INIT (bit 27, after GEMDOS init), which
  // happens once the ST is powered on. The callback computes the
  // effective relocation address / _memtop value from aconfig overrides
  // (or screen_base - 8 KB by default) and publishes them via shared
  // variables before send_sync's random-token ack returns to the m68k.
  gemdrive_init();

  // Register the Runner's chandler callback — receives
  // RUNNER_CMD_DONE_EXECUTE (and future report-back commands) the
  // m68k Runner publishes via send_sync.
  runner_init();

  // After this point, the remote computer can execute the code

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.
  // If you want to use a custom app in the remote computer, you can do it.
  // But it's easier to debug and code in the rp2040

  // Initialize the display
  display_setupU8g2();

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues and reports SD status in the terminal menu.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/test";  // MODIFY THIS TO YOUR FOLDER NAME
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
    // Also ensure the GEMDRIVE folder exists. When the user boots a
    // fresh card, the directory configured by GEMDRIVE_FOLDER (default
    // "/devops") is missing, and Fsfirst / Fopen fail with FR_NO_PATH
    // even though the SD itself is mounted. Mirrors what the
    // md-drives-emulator firmware does at boot.
    SettingsConfigEntry *gemdriveFolder = settings_find_entry(
        aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
    const char *gemdriveFolderName =
        (gemdriveFolder != NULL) ? gemdriveFolder->value : "/devops";
    sdcard_status_t gemdriveStatus = sdcard_ensureFolder(gemdriveFolderName);
    if (gemdriveStatus != SDCARD_INIT_OK) {
      DPRINTF("GEMDRIVE folder '%s' could not be ensured (status %i)\n",
              gemdriveFolderName, gemdriveStatus);
    } else {
      DPRINTF("GEMDRIVE folder '%s' ready.\n", gemdriveFolderName);
    }
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // Pre-init the stuff
  // In this example it only prints the please wait message, but can be used as
  // a place to put other code that needs to be run before the network is
  // initialized
  preinit();

  // 6. Init the network, if needed
  // It's always a good idea to wait for the network to be ready
  // Get the WiFi mode from the settings
  // If you are developing code that does not use the network, you can
  // comment this section
  // It's important to note that the network parameters are taken from the
  // global configuration of the Booster app. The network parameters are
  // ready only for the microfirmware apps.
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  wifi_mode_t wifiModeValue = WIFI_MODE_STA;
  if (wifiMode == NULL) {
    DPRINTF("No WiFi mode found in the settings. No initializing.\n");
  } else {
    wifiModeValue = (wifi_mode_t)atoi(wifiMode->value);
    if (wifiModeValue != WIFI_MODE_AP) {
      DPRINTF("WiFi mode is STA\n");
      wifiModeValue = WIFI_MODE_STA;
      int err = network_wifiInit(wifiModeValue);
      if (err != 0) {
        DPRINTF("Error initializing the network: %i. No initializing.\n", err);
      } else {
        // Drain commands and run the terminal loop during WiFi polling so
        // commands sent during the (potentially multi-second) connect don't
        // pile up in the ROM3 ring.
        network_setPollingCallback(emul_pollTick);
        // Connect to the WiFi network
        int maxAttempts = 3;  // or any other number defined elsewhere
        int attempt = 0;
        err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;

        while ((attempt < maxAttempts) &&
               (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
          err = network_wifiStaConnect();
          attempt++;

          if ((err > 0) && (err < NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
            DPRINTF("Error connecting to the WiFi network: %i\n", err);
          }
        }

        if (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT) {
          DPRINTF("Timeout connecting to the WiFi network after %d attempts\n",
                  maxAttempts);
          // Optionally, return an error code here.
        }
        network_setPollingCallback(NULL);

        // Remote HTTP Management API (Epic 02). Started right after
        // Wi-Fi association so it's reachable from the menu phase.
        // Earlier builds deferred this to firmware launch because of
        // an ST-side crash; that turned out to be RAM pressure (the
        // per-conn pool was 47 KB of BSS, pushing the heap into the
        // ROM-in-RAM region). After shrinking the pool to ~5 KB and
        // moving every http_server function to RAM via
        // __not_in_flash_func, running the server during the menu
        // is safe. Idempotent — safe even if Wi-Fi connect timed out.
        http_server_init();
      }
    } else {
      DPRINTF("WiFi mode is AP. No initializing.\n");
    }
  }

  // 7. Configure the SELECT button so menu status can show it immediately.
  select_configure();

  // 8. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init();

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to exit.
  // For testing purposes, this app only shows commands to manage the settings
  DPRINTF("Start the app loop here\n");
  lastCountdownTick = get_absolute_time();
  while (getKeepActive()) {
#if PICO_CYW43_ARCH_POLL
    network_safePoll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(SLEEP_LOOP_MS));
#else
    sleep_ms(SLEEP_LOOP_MS);
#endif
    // Drain the ROM3 command ring → dispatch to registered callbacks.
    chandler_loop();

    // Pump pending debug bytes out the USB CDC interface
    // (Epic 05 v2 / S5). Cheap when no host is attached or the
    // debugcap ring is empty.
    usbcdc_drain();

    // Run the terminal foreground (consume the published command, render
    // output, etc.).
    term_loop();

    // Runner-mode auto-relaunch. Single-shot ticker firing once at
    // +3 s after `runner reset` was racy — if the ST hadn't yet
    // reached its check_commands poll loop the sentinel write was
    // missed (or got overwritten by a still-in-flight RUNNER_CMD_RESET
    // dispatch). Replaced with a self-correcting retry: while
    // runnerRelaunchAtMs is non-zero we re-fire
    // DISPLAY_COMMAND_START_RUNNER every ~500 ms until the m68k
    // Runner's RUNNER_CMD_DONE_HELLO callback (runner.c) lands and
    // calls emul_scheduleRunnerRelaunch(0) to cancel us. Idempotent —
    // writing 5 to the sentinel multiple times is harmless because
    // check_commands only reads it. Stops as soon as the runner is
    // confirmed back on the air.
    if (runnerRelaunchAtMs != 0 && runnerActive) {
      uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
      if ((int32_t)(now_ms - runnerRelaunchAtMs) >= 0) {
        DPRINTF("emul: relaunch tick — firing DISPLAY_COMMAND_START_RUNNER\n");
        SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START_RUNNER);
        runnerRelaunchAtMs = now_ms + 500;  // try again in 500 ms
      }
    }

    // Any keystroke (bound OR unbound) halts the autoboot countdown,
    // matching md-drives-emulator. Bound keys also trigger their
    // command handler which sets haltCountdown = true on its own;
    // for unbound keys the keystroke flag from term.c is the only
    // signal, so we poll it here and set the flag.
    if (!haltCountdown && term_consumeAnyKeyPressed()) {
      haltCountdown = true;
    }

    // When haltCountdown flips false → true repaint the bottom strip
    // once with the "Countdown stopped" message — otherwise the strip
    // would freeze on whatever counter value it last drew, since
    // showCounter is only called inside the decrement branch below.
    static bool lastHaltState = false;
    if (haltCountdown && !lastHaltState) {
      drawSetupInfoLine("Countdown stopped. Press [E], [U] or [X] to continue.");
      display_refresh();
    }
    lastHaltState = haltCountdown;

    if (!haltCountdown && menuScreenActive) {
      absolute_time_t now = get_absolute_time();
      if (absolute_time_diff_us(lastCountdownTick, now) >= 1000000) {
        lastCountdownTick = now;
        countdown--;
        showCounter(countdown);
        display_refresh();
        if (countdown <= 0) {
          haltCountdown = true;
          // Autoboot expired — launch DevOps Runner on the Atari ST. Same
          // path as pressing [U] (Epic 06 / S2). Runner is the more useful
          // default: it includes the [F]/[E] GEMDRIVE behaviour AND the
          // workstation-driven Runner control surface.
          cmdRunner(NULL);
        }
      }
    }

    // Epic 06 / S4: live-refresh the USB CDC status line on the
    // menu (independent of countdown halt state — the user should
    // see the plug/unplug transition whether they've stopped the
    // counter or not).
    refreshUsbCdcLine();
  }

  // 10. Send RESET computer command
  // Ok, so we are done with the setup but we want to reset the computer to
  // reboot in the same microfirmware app or start the booster app

  sleep_ms(SLEEP_LOOP_MS);
  // We must reset the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(SLEEP_LOOP_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Set emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         APP_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Jump to the booster app
    DPRINTF("Jumping to the booster app...\n");
    reset_jump_to_booster();
  }
}
