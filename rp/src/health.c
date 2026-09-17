/**
 * File: health.c
 * Author: Diego Parrilla Santamaría
 * Date: September 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Device health: stack and heap high-water marks, crash
 *              and hang recovery through the watchdog, and the reset
 *              reason kept across the reboot. See health.h.
 */

#include "include/health.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "build_id.h"
#include "commemul.h"
#include "debug.h"
#include "hardware/structs/scb.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

// --- Breadcrumb layout in watchdog scratch[0] ---------------------------

#define HEALTH_MAGIC 0xD5u
#define S0_MAGIC(v) (((v) >> 24) & 0xFFu)
#define S0_REASON(v) (((v) >> 20) & 0x0Fu)
#define S0_COUNT(v) (((v) >> 16) & 0x0Fu)
#define S0_WINDOW(v) (((v) >> 8) & 0xFFu)
#define S0_PHASE(v) ((v) & 0xFFu)
#define S0_MAKE(reason, count, window, phase)          \
  ((HEALTH_MAGIC << 24) | (((reason) & 0x0Fu) << 20) | \
   (((count) & 0x0Fu) << 16) | (((window) & 0xFFu) << 8) | ((phase) & 0xFFu))

// What the running firmware leaves in the reason field.
#define REASON_RUNNING 0u
#define REASON_RESET 1u
#define REASON_PANIC 2u
#define REASON_HARDFAULT 3u

// Stack paint pattern.
#define STACK_PAINT 0x57AC57ACu
// Words left unpainted below the current SP when painting.
#define STACK_PAINT_MARGIN_WORDS 16u
// The SDK's MPU stack guard (PICO_USE_STACK_GUARDS) protects the lowest 32
// bytes of the stack, rounded up to a 32-byte boundary. Painting or scanning
// them would fault, so the paint starts above the guard.
#if PICO_USE_STACK_GUARDS
#define STACK_GUARD_BYTES 32u
#else
#define STACK_GUARD_BYTES 0u
#endif

#define HEAP_SAMPLE_INTERVAL_MS 100u
#define WINDOW_UPDATE_INTERVAL_MS 1000u
#define SUMMARY_INTERVAL_MS 30000u
#define CRASH_REBOOT_DELAY_MS 100u
// Stall mark: a timer interrupt writes it to scratch[1] once the watchdog
// has gone this long without a feed, and clears it when feeding resumes.
// A watchdog reset only counts as a hang if the mark is there: a debug
// probe resets the chip with SYSRESETREQ, which leaves the watchdog
// reason and scratch registers exactly as a real expiry does.
#define STALL_MAGIC 0x57A11ED0u
#define STALL_CHECK_MS 1000u
#define STALL_MARK_MS 5000u
// Vector table slot of the HardFault exception.
#define HEALTH_HARDFAULT_VECTOR 3u

extern char end;
extern char __StackLimit;
extern char __StackTop;
extern char __StackBottom;
extern char __data_start__;
extern char __data_end__;

static health_boot_t bootCause = HEALTH_BOOT_POWER_ON;
static uint8_t bootPhase = 0;
static uint32_t bootPc = 0;
static uint32_t bootLr = 0;
static uint32_t bootSp = 0;
static uint8_t crashCount = 0;
// Seconds of the crash window already used by previous boots.
static uint32_t crashWindowBaseS = 0;
static bool watchdogEnabled = false;
static uint32_t feedCount = 0;
static uint32_t stallLastFeedCount = 0;
static uint32_t stallMs = 0;
static repeating_timer_t stallTimer;

static uint32_t heapMinFree = UINT32_MAX;
static uint32_t sbrkHighWater = 0;
static uint32_t lastHeapSampleMs = 0;
static uint32_t lastWindowUpdateMs = 0;
#if defined(_DEBUG) && (_DEBUG != 0)
static uint32_t lastSummaryMs = 0;
#endif

static uint32_t health_nowMs(void) {
  return to_ms_since_boot(get_absolute_time());
}

// --- Stack painting ------------------------------------------------------

// Paint [from, to) with the pattern, word-aligned.
static void health_paintRange(uint32_t from, uint32_t to) {
  from = (from + 3u) & ~3u;
  to &= ~3u;
  for (uint32_t a = from; a < to; a += 4u) {
    *(uint32_t *)a = STACK_PAINT;
  }
}

// The core-0 stack is a plain region at the top of main RAM now, so
// paint all of it below the current stack pointer. No code lives in it, unlike
// the scratch banks it used to share.
static uint32_t health_stackPaintFloor(void) {
  return (((uint32_t)&__StackBottom + STACK_GUARD_BYTES) + 31u) & ~31u;
}

static __attribute__((noinline)) void health_paintStack(void) {
  uint32_t here = 0;
  uint32_t sp = (uint32_t)&here - STACK_PAINT_MARGIN_WORDS * 4u;
  health_paintRange(health_stackPaintFloor(), sp);
}

// Lowest address below __StackTop that no longer holds the pattern.
static uint32_t health_stackLowestTouched(void) {
  uint32_t a = health_stackPaintFloor();
  uint32_t top = (uint32_t)&__StackTop;
  for (; a < top; a += 4u) {
    if (*(uint32_t *)a != STACK_PAINT) return a;
  }
  return top;
}

// --- Heap ----------------------------------------------------------------

static void health_sampleHeap(uint32_t *freeOut, uint32_t *brkUsedOut) {
  struct mallinfo mi = mallinfo();
  uint32_t brk = (uint32_t)sbrk(0);
  uint32_t limit = (uint32_t)&__StackLimit;
  uint32_t heapFree = (limit > brk ? limit - brk : 0) + (uint32_t)mi.fordblks;
  uint32_t brkUsed = brk - (uint32_t)&end;
  if (heapFree < heapMinFree) heapMinFree = heapFree;
  if (brkUsed > sbrkHighWater) sbrkHighWater = brkUsed;
  if (freeOut) *freeOut = heapFree;
  if (brkUsedOut) *brkUsedOut = brkUsed;
}

// --- Boot ----------------------------------------------------------------

static void health_installHardfault(void);

void health_init(void) {
  uint32_t s0 = watchdog_hw->scratch[0];
  bool fromWatchdog = watchdog_caused_reboot();
  bool ours = S0_MAGIC(s0) == HEALTH_MAGIC;

  if (!fromWatchdog) {
    bootCause = HEALTH_BOOT_POWER_ON;
  } else if (!ours) {
    bootCause = HEALTH_BOOT_REBOOT;
  } else {
    switch (S0_REASON(s0)) {
      case REASON_RESET:
        bootCause = HEALTH_BOOT_RESET;
        break;
      case REASON_PANIC:
        bootCause = HEALTH_BOOT_PANIC;
        break;
      case REASON_HARDFAULT:
        bootCause = HEALTH_BOOT_HARDFAULT;
        break;
      default:
        // Still "running" when the reset came. Only the stall mark proves
        // the watchdog expired: a probe reset or flash looks the same in
        // every register. A hang with interrupts off cannot set the mark
        // and is reported as a reboot.
        bootCause = (watchdog_enable_caused_reboot() &&
                     watchdog_hw->scratch[1] == STALL_MAGIC)
                        ? HEALTH_BOOT_HANG
                        : HEALTH_BOOT_REBOOT;
        break;
    }
  }

  bool crashed = bootCause == HEALTH_BOOT_PANIC ||
                 bootCause == HEALTH_BOOT_HARDFAULT ||
                 bootCause == HEALTH_BOOT_HANG;
  if (crashed) {
    bootPhase = (uint8_t)S0_PHASE(s0);
    bootPc = watchdog_hw->scratch[1];
    bootLr = watchdog_hw->scratch[2];
    bootSp = watchdog_hw->scratch[3];
    uint32_t prevCount = S0_COUNT(s0);
    uint32_t prevWindow = S0_WINDOW(s0);
    if (prevCount >= HEALTH_CRASH_LOOP_COUNT) {
      // Tripped: stays tripped until a SELECT reset or a power cycle.
      crashCount = (uint8_t)(prevCount < 15u ? prevCount + 1u : 15u);
      crashWindowBaseS = 0;
    } else if (prevCount > 0 && prevWindow <= HEALTH_CRASH_LOOP_WINDOW_S) {
      crashCount = (uint8_t)(prevCount + 1u);
      crashWindowBaseS = prevWindow;
    } else {
      crashCount = 1;
      crashWindowBaseS = 0;
    }
  } else {
    crashCount = 0;
    crashWindowBaseS = 0;
  }

  watchdog_hw->scratch[0] =
      S0_MAKE(REASON_RUNNING, crashCount, crashWindowBaseS, HEALTH_PHASE_BOOT);
  watchdog_hw->scratch[1] = 0;
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[3] = 0;

  health_installHardfault();
  health_paintStack();
  health_sampleHeap(NULL, NULL);

  DPRINTF("health: build %s, boot cause %s", RELEASE_BUILD_ID,
          health_bootName(bootCause));
  if (bootCause == HEALTH_BOOT_HANG) {
    DPRINTFRAW(" in %s", health_phaseName(bootPhase));
  } else if (bootCause == HEALTH_BOOT_PANIC ||
             bootCause == HEALTH_BOOT_HARDFAULT) {
    DPRINTFRAW(" pc=0x%08lx lr=0x%08lx sp=0x%08lx", (unsigned long)bootPc,
               (unsigned long)bootLr, (unsigned long)bootSp);
  }
  DPRINTFRAW(" (crashes %u%s)\n", (unsigned)crashCount,
             health_isCrashLoop() ? ", crash loop: countdown stopped" : "");
}

// Timer interrupt, once a second: marks a stalled feed (see STALL_MAGIC).
// scratch[1] is shared with the crash handlers, which store the PC there just
// before the reboot. This timer can still run in that window, so it only
// replaces its own values (0 or the mark), never a crash PC.
static bool health_stallCheck(repeating_timer_t *timer) {
  (void)timer;
  if (feedCount != stallLastFeedCount) {
    stallLastFeedCount = feedCount;
    stallMs = 0;
    if (watchdog_hw->scratch[1] == STALL_MAGIC) watchdog_hw->scratch[1] = 0;
  } else if (stallMs < STALL_MARK_MS) {
    stallMs += STALL_CHECK_MS;
    if (stallMs >= STALL_MARK_MS && watchdog_hw->scratch[1] == 0) {
      watchdog_hw->scratch[1] = STALL_MAGIC;
    }
  }
  return true;
}

void health_watchdogStart(void) {
  watchdog_enable(HEALTH_WATCHDOG_TIMEOUT_MS, true);
  watchdogEnabled = true;
  add_repeating_timer_ms((int32_t)STALL_CHECK_MS, health_stallCheck, NULL,
                         &stallTimer);
  DPRINTF("health: watchdog enabled, %u ms\n",
          (unsigned)HEALTH_WATCHDOG_TIMEOUT_MS);
}

void health_feed(void) {
  if (watchdogEnabled) {
    watchdog_update();
    feedCount++;
  }
}

void health_markReset(void) {
  watchdog_hw->scratch[0] = S0_MAKE(REASON_RESET, 0, 0, HEALTH_PHASE_BOOT);
}

void health_prepareJump(void) {
  if (watchdogEnabled) cancel_repeating_timer(&stallTimer);
  watchdog_disable();
  watchdogEnabled = false;
  watchdog_hw->scratch[0] = 0;
}

bool health_isCrashLoop(void) { return crashCount >= HEALTH_CRASH_LOOP_COUNT; }

// --- Crash handlers ------------------------------------------------------

static void health_crashReboot(uint32_t reason) {
  uint32_t s0 = watchdog_hw->scratch[0];
  watchdog_hw->scratch[0] =
      S0_MAKE(reason, S0_COUNT(s0), S0_WINDOW(s0), S0_PHASE(s0));
  // Arm the reboot before printing, so a console that cannot print from
  // here (a held stdio lock) still ends in a reboot.
  watchdog_reboot(0, 0, CRASH_REBOOT_DELAY_MS);
}

// Called from the panic shim below with the caller's PC and SP already
// in scratch[1] and scratch[3].
void __attribute__((noreturn)) health_panicReport(const char *fmt, ...) {
  watchdog_hw->scratch[2] = 0;
  health_crashReboot(REASON_PANIC);
#if defined(_DEBUG) && (_DEBUG != 0)
  fprintf(stderr, "\n*** PANIC ***\n");
  if (fmt != NULL) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
  }
  fprintf(stderr, "\npc=0x%08lx sp=0x%08lx. Rebooting.\n",
          (unsigned long)watchdog_hw->scratch[1],
          (unsigned long)watchdog_hw->scratch[3]);
#else
  (void)fmt;
#endif
  for (;;) tight_loop_contents();
}

// PICO_PANIC_FUNCTION. The SDK's panic() is `push {lr}; bl health_panic`,
// so the address that called panic() is at [sp] on entry. Record it and
// the caller's SP before any C code moves the stack, then continue in
// health_panicReport with the arguments untouched.
void __attribute__((naked, noreturn)) health_panic(const char *fmt, ...) {
  pico_default_asm(
      "push {r4, r5}\n"
      "ldr r4, =0x40058010\n"  // &watchdog_hw->scratch[1]
      "ldr r5, [sp, #8]\n"     // return address pushed by panic()
      "subs r5, #5\n"          // back to the bl (4 bytes) and clear thumb
      "str r5, [r4, #0]\n"     // scratch[1] = PC
      "mov r5, sp\n"
      "adds r5, #12\n"      // SP of the caller when it called panic()
      "str r5, [r4, #8]\n"  // scratch[3] = SP
      "ldr r4, =health_panicReport\n"
      "mov r12, r4\n"
      "pop {r4, r5}\n"
      "bx r12\n");
}

void __attribute__((noreturn)) health_hardfaultReport(uint32_t *frame) {
  // Exception frame: r0 r1 r2 r3 r12 lr pc xpsr.
  watchdog_hw->scratch[1] = frame[6];
  watchdog_hw->scratch[2] = frame[5];
  watchdog_hw->scratch[3] = (uint32_t)frame + 32u;
  health_crashReboot(REASON_HARDFAULT);
#if defined(_DEBUG) && (_DEBUG != 0)
  fprintf(stderr,
          "\n*** HARDFAULT ***\npc=0x%08lx lr=0x%08lx sp=0x%08lx. "
          "Rebooting.\n",
          (unsigned long)frame[6], (unsigned long)frame[5],
          (unsigned long)((uint32_t)frame + 32u));
#endif
  for (;;) tight_loop_contents();
}

// HardFault entry. Picks the stack the exception frame was pushed to and
// hands it to C. Installed at boot by health_installHardfault.
static void __attribute__((naked)) health_hardfault(void) {
  pico_default_asm(
      "movs r0, #4\n"
      "mov r1, lr\n"
      "tst r0, r1\n"
      "bne 1f\n"
      "mrs r0, msp\n"
      "b 2f\n"
      "1: mrs r0, psp\n"
      "2: ldr r1, =health_hardfaultReport\n"
      "bx r1\n");
}

// fatfs-sdk's crash.c already defines isr_hardfault, and its handler
// stops at a breakpoint, which locks up an RP2040 with no debugger. So
// replace the entry in the RAM vector table at runtime instead of at
// link time, keeping the submodule untouched.
static void health_installHardfault(void) {
  uint32_t vtor = scb_hw->vtor;
  if ((vtor & 0xF0000000u) != 0x20000000u) {
    DPRINTF("health: vector table not in RAM, HardFault handler not set\n");
    return;
  }
  ((void (**)(void))vtor)[HEALTH_HARDFAULT_VECTOR] = health_hardfault;
}

// --- Main loop -----------------------------------------------------------

#if defined(_DEBUG) && (_DEBUG != 0)
static health_test_t pendingTest = HEALTH_TEST_NONE;
static uint32_t pendingTestAtMs = 0;

#define TEST_DELAY_MS 250u
#define TEST_STALL_MS 500u

void health_requestTest(health_test_t test) {
  pendingTestAtMs = health_nowMs() + TEST_DELAY_MS;
  pendingTest = test;
}

// Recurse with a frame big enough to reach the guard quickly. The frame has to
// escape into a volatile sink and be used again after the call, or GCC turns
// this accumulator recursion into a plain loop and the stack never grows (it
// did: the first version ran to completion with a 2,136-byte high-water).
// The POINTER must be volatile, not what it points at: with `volatile uint32_t
// *sink` the stores were dead code, the call became a tail call and GCC
// rewrote the recursion as a loop over one frame.
static volatile uint32_t *volatile healthBurnSink;

static __attribute__((noinline)) void health_burnStack(uint32_t depth) {
  volatile uint32_t block[64];
  for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++) {
    block[i] = depth + (uint32_t)i;
  }
  healthBurnSink = block;
  if (depth < 100000u) {
    health_burnStack(depth + 1u);
  }
  healthBurnSink = block;
}

static void health_runPendingTest(uint32_t now) {
  if (pendingTest == HEALTH_TEST_NONE || now < pendingTestAtMs) return;
  health_test_t test = pendingTest;
  pendingTest = HEALTH_TEST_NONE;
  switch (test) {
    case HEALTH_TEST_PANIC:
      DPRINTF("health: test panic\n");
      panic("health test panic");
      break;
    case HEALTH_TEST_HARDFAULT: {
      DPRINTF("health: test HardFault\n");
      // Nothing is mapped here.
      volatile uint32_t v = *(volatile uint32_t *)0xF0000000u;
      (void)v;
      break;
    }
    case HEALTH_TEST_HANG:
      DPRINTF("health: test hang in the main loop\n");
      for (;;) tight_loop_contents();
      break;
    case HEALTH_TEST_STALL:
      DPRINTF("health: test stall %u ms\n", (unsigned)TEST_STALL_MS);
      busy_wait_ms(TEST_STALL_MS);
      break;
    case HEALTH_TEST_STACK_OVERFLOW:
      DPRINTF("health: test stack overflow, stack bottom 0x%08lx\n",
              (unsigned long)(uint32_t)&__StackBottom);
      health_burnStack(0);
      break;
    default:
      break;
  }
}
#endif

void health_tick(void) {
  uint32_t now = health_nowMs();

  if (now - lastHeapSampleMs >= HEAP_SAMPLE_INTERVAL_MS) {
    lastHeapSampleMs = now;
    health_sampleHeap(NULL, NULL);
  }

  // Keep the crash window current, so a hang (which runs no handler)
  // still reboots with the right window.
  if (crashCount > 0 && now - lastWindowUpdateMs >= WINDOW_UPDATE_INTERVAL_MS) {
    lastWindowUpdateMs = now;
    uint32_t window = crashWindowBaseS + now / 1000u;
    if (window > 255u) window = 255u;
    uint32_t s0 = watchdog_hw->scratch[0];
    watchdog_hw->scratch[0] =
        S0_MAKE(S0_REASON(s0), crashCount, window, S0_PHASE(s0));
  }

#if defined(_DEBUG) && (_DEBUG != 0)
  if (now - lastSummaryMs >= SUMMARY_INTERVAL_MS) {
    lastSummaryMs = now;
    health_report_t r;
    health_getReport(&r);
    DPRINTF(
        "health: up %lus heap free %lu min %lu brk %lu stack %lu/%lu%s "
        "rom3 overruns %lu boot %s\n",
        (unsigned long)(r.uptime_ms / 1000u), (unsigned long)r.heap_free,
        (unsigned long)r.heap_min_free, (unsigned long)r.sbrk_high_water,
        (unsigned long)r.stack_high_water, (unsigned long)r.stack_reserved,
        r.stack_overflow ? " OVERFLOW" : "",
        (unsigned long)commemul_getOverruns(), health_bootName(r.boot));
  }
  health_runPendingTest(now);
#endif
}

// --- Reporting -----------------------------------------------------------

void health_getReport(health_report_t *out) {
  memset(out, 0, sizeof(*out));
  out->uptime_ms = health_nowMs();

  health_sampleHeap(&out->heap_free, NULL);
  out->heap_total = (uint32_t)&__StackLimit - (uint32_t)&end;
  out->heap_min_free = heapMinFree;
  out->sbrk_high_water = sbrkHighWater;

  uint32_t top = (uint32_t)&__StackTop;
  uint32_t lowestPainted = health_stackPaintFloor();
  uint32_t lowest = health_stackLowestTouched();
  out->stack_reserved = top - (uint32_t)&__StackBottom;
  out->stack_high_water = top - lowest;
  out->stack_painted = top - lowestPainted;
  out->stack_overflow = lowest <= lowestPainted;
  out->code_in_ram = (uint32_t)&__data_end__ - (uint32_t)&__data_start__;

  out->boot = bootCause;
  out->boot_phase = bootPhase;
  out->boot_pc = bootPc;
  out->boot_lr = bootLr;
  out->boot_sp = bootSp;
  out->crash_count = crashCount;
  out->crash_loop = health_isCrashLoop();
  out->watchdog_enabled = watchdogEnabled;
}

const char *health_bootName(health_boot_t boot) {
  switch (boot) {
    case HEALTH_BOOT_POWER_ON:
      return "power_on";
    case HEALTH_BOOT_RESET:
      return "reset";
    case HEALTH_BOOT_PANIC:
      return "panic";
    case HEALTH_BOOT_HARDFAULT:
      return "hardfault";
    case HEALTH_BOOT_HANG:
      return "hang";
    case HEALTH_BOOT_REBOOT:
      return "reboot";
    default:
      return "unknown";
  }
}

const char *health_phaseName(uint8_t phase) {
  switch (phase) {
    case HEALTH_PHASE_BOOT:
      return "boot";
    case HEALTH_PHASE_MAIN_LOOP:
      return "main_loop";
    case HEALTH_PHASE_WIFI_CONNECT:
      return "wifi_connect";
    case HEALTH_PHASE_HTTP_REQUEST:
      return "http_request";
    case HEALTH_PHASE_HTTP_WAIT:
      return "http_wait";
    case HEALTH_PHASE_GEMDRIVE:
      return "gemdrive";
    case HEALTH_PHASE_FLASH_WRITE:
      return "flash_write";
    default:
      return "unknown";
  }
}

bool health_getBootLine(char *buf, int size) {
  // The terminal is 40 columns wide: "Recovered: hang in http_request x15".
  char times[8] = "";
  if (crashCount > 1) snprintf(times, sizeof(times), " x%u", crashCount);
  switch (bootCause) {
    case HEALTH_BOOT_PANIC:
      snprintf(buf, (size_t)size, "Recovered: panic @%08lX%s",
               (unsigned long)bootPc, times);
      return true;
    case HEALTH_BOOT_HARDFAULT:
      snprintf(buf, (size_t)size, "Recovered: fault @%08lX%s",
               (unsigned long)bootPc, times);
      return true;
    case HEALTH_BOOT_HANG:
      snprintf(buf, (size_t)size, "Recovered: hang in %s%s",
               health_phaseName(bootPhase), times);
      return true;
    default:
      return false;
  }
}
