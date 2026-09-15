/**
 * File: health.h
 * Author: Diego Parrilla Santamaría
 * Date: September 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Device health: stack and heap high-water marks, crash
 *              and hang recovery through the watchdog, and the reset
 *              reason kept across the reboot.
 *
 *              The breadcrumb lives in watchdog scratch registers 0-3,
 *              which survive a watchdog reboot but not a power cycle.
 *              The SDK owns scratch 4-7.
 *
 *                scratch[0]  magic (8) | reason (4) | crash count (4) |
 *                            crash window s (8) | phase (8)
 *                scratch[1]  PC at the crash
 *                scratch[2]  LR at the crash (HardFault only)
 *                scratch[3]  SP at the crash
 */

#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/structs/watchdog.h"

// 8 s is just under the RP2040 maximum of 8,388 ms.
#define HEALTH_WATCHDOG_TIMEOUT_MS 8000

// Crash-loop guard: this many crash reboots within the window stop the
// boot countdown until a SELECT reset or a power cycle.
#define HEALTH_CRASH_LOOP_COUNT 3
#define HEALTH_CRASH_LOOP_WINDOW_S 60

// Where the firmware was when a hang fired the watchdog.
typedef enum {
  HEALTH_PHASE_BOOT = 0,
  HEALTH_PHASE_MAIN_LOOP = 1,
  HEALTH_PHASE_WIFI_CONNECT = 2,
  HEALTH_PHASE_HTTP_REQUEST = 3,
  HEALTH_PHASE_HTTP_WAIT = 4,
  HEALTH_PHASE_GEMDRIVE = 5,
  HEALTH_PHASE_FLASH_WRITE = 6,
} health_phase_t;

// Why the RP started this time.
typedef enum {
  HEALTH_BOOT_POWER_ON = 0,  // power-on, RUN pin or debugger reset
  HEALTH_BOOT_RESET,         // SELECT button or a menu reset
  HEALTH_BOOT_PANIC,
  HEALTH_BOOT_HARDFAULT,
  HEALTH_BOOT_HANG,    // the watchdog fired
  HEALTH_BOOT_REBOOT,  // any other watchdog reboot (Booster, picotool)
} health_boot_t;

typedef struct {
  uint32_t uptime_ms;
  // Heap: [end, __StackLimit).
  uint32_t heap_total;
  uint32_t heap_free;
  uint32_t heap_min_free;
  uint32_t sbrk_high_water;  // bytes above `end` sbrk ever reached
  // Core-0 stack.
  uint32_t stack_reserved;    // __StackTop - __StackBottom
  uint32_t stack_high_water;  // deepest use seen since boot
  uint32_t stack_painted;     // bytes below __StackTop that are measured
  bool stack_overflow;        // the lowest painted word was overwritten
  uint32_t code_in_ram;       // __data_end__ - __data_start__
  // Why this boot happened.
  health_boot_t boot;
  uint8_t boot_phase;   // valid for HEALTH_BOOT_HANG
  uint32_t boot_pc;     // valid for PANIC and HARDFAULT
  uint32_t boot_lr;     // valid for HARDFAULT
  uint32_t boot_sp;     // valid for PANIC and HARDFAULT
  uint8_t crash_count;  // crash reboots in the current window
  bool crash_loop;
  bool watchdog_enabled;
} health_report_t;

// Call first thing in emul_start: decodes the previous reset, updates
// the crash-loop guard and paints the stack. Must run before
// health_watchdogStart, which overwrites the SDK's reboot marker.
void health_init(void);

// Enable the watchdog. Feed it only with health_feed, from the main
// loop and the few long waits that are known to make progress.
void health_watchdogStart(void);

void health_feed(void);

// Record the current phase in the breadcrumb. Cheap: one register write.
static inline void health_setPhase(health_phase_t phase) {
  watchdog_hw->scratch[0] = (watchdog_hw->scratch[0] & ~0xFFu) | (uint8_t)phase;
}

// Before a deliberate reboot (SELECT, menu reset): the next boot reports
// a reset and clears the crash-loop guard.
void health_markReset(void);

// Before jumping to Booster: stop the watchdog and clear the breadcrumb.
void health_prepareJump(void);

// Main loop: samples the heap, keeps the crash window current, prints
// the debug summary and runs a pending debug test hook.
void health_tick(void);

void health_getReport(health_report_t *out);

bool health_isCrashLoop(void);

const char *health_bootName(health_boot_t boot);
const char *health_phaseName(uint8_t phase);

// One short line for the setup menu, or false when the last reset was
// ordinary (power-on or a reset the user asked for).
bool health_getBootLine(char *buf, int size);

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug-only fault injection, used to verify recovery on hardware.
typedef enum {
  HEALTH_TEST_NONE = 0,
  HEALTH_TEST_PANIC,
  HEALTH_TEST_HARDFAULT,
  HEALTH_TEST_HANG,   // hang in the main loop
  HEALTH_TEST_STALL,  // stop the main loop for 500 ms
} health_test_t;

// Runs the test from the main loop after a short delay, so the HTTP
// response that requested it gets out first.
void health_requestTest(health_test_t test);
#endif

#endif  // HEALTH_H
