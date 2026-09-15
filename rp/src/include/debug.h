/**
 * File: debug.h
 * Author: Diego Parrilla Santamaría
 * Date: July 2023, February 2026
 * Copyright: 2023-2026 - GOODDATA LABS SL
 * Description: Header file for basic traces and debug messages
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "pico/stdlib.h"

// DEBUG_BUFFERED_CONSOLE selects how DPRINTF reaches the UART console in
// debug builds. Override it with -DDEBUG_BUFFERED_CONSOLE=0 or =1.
//
//   0  DPRINTF writes straight to stderr and waits until every byte has left
//      the UART. Every line reaches the console, even the last ones before a
//      HardFault or a hang, but each line stalls the caller for about 1 ms
//      at 921,600 baud.
//   1  DPRINTF copies the text into a RAM ring buffer and returns at once;
//      the UART transmit interrupt sends it at full speed. The caller no
//      longer stalls. stderr and stdout are redirected to the same queue.
//      Text written from an exception (a HardFault report, an interrupt
//      handler) is sent at once, after what is queued. When the buffer is
//      full the caller waits for room, so nothing is lost; bursts such as
//      the boot settings dumps still stall briefly. Costs: a 4 KB buffer in
//      RAM, and text still queued when a hang happens is lost.
//
// Use 0 when chasing a crash or a hang, 1 when traces disturb timing.
//
// The definitions below replace any DPRINTF or DPRINTFRAW defined earlier,
// such as the settings library's fallback in settings.h, so every file that
// includes this header uses this implementation.
#ifndef DEBUG_BUFFERED_CONSOLE
#define DEBUG_BUFFERED_CONSOLE 1
#endif

#if defined(_DEBUG) && (_DEBUG != 0) && (DEBUG_BUFFERED_CONSOLE != 0)

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define DEBUG_CONSOLE_RING_BYTES 4096u  // a power of two
#define DEBUG_CONSOLE_RING_MASK (DEBUG_CONSOLE_RING_BYTES - 1u)

// State and functions shared by every file that includes this header. Weak
// definitions are merged by the linker into a single copy of each.
__attribute__((weak)) uint8_t debugConsoleRing[DEBUG_CONSOLE_RING_BYTES];
__attribute__((weak)) uint32_t debugConsoleHead;  // next byte to queue
__attribute__((weak)) uint32_t debugConsoleTail;  // next byte to send
__attribute__((weak)) FILE *debugConsoleFile;
__attribute__((weak)) bool debugConsoleStarted;

// Move queued bytes into the UART FIFO, and keep the transmit interrupt on
// only while bytes remain. Call with interrupts disabled.
__attribute__((weak, noinline)) void debug_consoleFill(void) {
  uart_hw_t *hw = uart_get_hw(uart_default);
  while (debugConsoleTail != debugConsoleHead &&
         !(hw->fr & UART_UARTFR_TXFF_BITS)) {
    hw->dr = debugConsoleRing[debugConsoleTail & DEBUG_CONSOLE_RING_MASK];
    debugConsoleTail++;
  }
  if (debugConsoleTail == debugConsoleHead) {
    hw_clear_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    hw->icr = UART_UARTICR_TXIC_BITS;
  } else {
    hw_set_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
  }
}

__attribute__((weak, noinline)) void debug_consoleIrq(void) {
  debug_consoleFill();
}

// Send the oldest queued byte now, waiting for room in the UART FIFO. Call
// with interrupts disabled.
__attribute__((weak, noinline)) void debug_consoleSendOne(void) {
  uart_hw_t *hw = uart_get_hw(uart_default);
  while (hw->fr & UART_UARTFR_TXFF_BITS) tight_loop_contents();
  hw->dr = debugConsoleRing[debugConsoleTail & DEBUG_CONSOLE_RING_MASK];
  debugConsoleTail++;
}

// funopen write callback: queue the text and start sending. Nothing is ever
// dropped: when the ring is full the caller waits for the UART interrupt to
// make room, or, with interrupts off or inside an exception, sends bytes
// itself.
__attribute__((weak, noinline)) int debug_consoleWrite(void *cookie,
                                                       const char *buf, int n) {
  (void)cookie;
  uint32_t save = save_and_disable_interrupts();
  bool inException = __get_current_exception() != 0;
  if (inException) {
    // The UART interrupt cannot run here: send what is queued and this text
    // now, so a crash report always reaches the console, in order.
    while (debugConsoleTail != debugConsoleHead) debug_consoleSendOne();
    uart_hw_t *hw = uart_get_hw(uart_default);
    hw_clear_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    for (int i = 0; i < n; i++) {
      while (hw->fr & UART_UARTFR_TXFF_BITS) tight_loop_contents();
      hw->dr = (uint8_t)buf[i];
    }
    restore_interrupts(save);
    return n;
  }
  bool interruptsWereOn = (save & 1u) == 0u;
  for (int i = 0; i < n;) {
    if (debugConsoleHead - debugConsoleTail < DEBUG_CONSOLE_RING_BYTES) {
      debugConsoleRing[debugConsoleHead & DEBUG_CONSOLE_RING_MASK] =
          (uint8_t)buf[i++];
      debugConsoleHead++;
    } else if (interruptsWereOn) {
      debug_consoleFill();
      restore_interrupts(save);
      tight_loop_contents();  // the UART interrupt drains the ring here
      save = save_and_disable_interrupts();
    } else {
      debug_consoleSendOne();
    }
  }
  debug_consoleFill();
  restore_interrupts(save);
  return n;
}

// The FILE DPRINTF writes to. Until the UART is up (stdio_init_all in
// main.c) it falls back to stderr. Starting it redirects stderr and stdout.
__attribute__((weak, noinline)) FILE *debug_console(void) {
  if (debugConsoleStarted) return debugConsoleFile;
  if (!uart_is_enabled(uart_default)) return stderr;
  FILE *f = fwopen(NULL, debug_consoleWrite);
  if (f == NULL) return stderr;
  setvbuf(f, NULL, _IONBF, 0);
  debugConsoleFile = f;
  // Every other writer too (the settings library's own DPRINTF, crash
  // reports, printf) goes through the same queue, so nothing writes to the
  // UART behind the interrupt's back: two writers would overrun the FIFO.
  stderr = f;
  stdout = f;
  irq_add_shared_handler(UART_IRQ_NUM(uart_default), debug_consoleIrq,
                         PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(UART_IRQ_NUM(uart_default), true);
  debugConsoleStarted = true;
  return debugConsoleFile;
}

#undef DPRINTF
#define DPRINTF(fmt, ...)                                                     \
  do {                                                                        \
    const char *file =                                                        \
        strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__;       \
    fprintf(debug_console(), "%s:%d:%s(): " fmt "", file, __LINE__, __func__, \
            ##__VA_ARGS__);                                                   \
  } while (0)
#undef DPRINTFRAW
#define DPRINTFRAW(fmt, ...)                      \
  do {                                            \
    fprintf(debug_console(), fmt, ##__VA_ARGS__); \
  } while (0)

#elif defined(_DEBUG) && (_DEBUG != 0)

/**
 * @brief A macro to print debug
 *
 * @param fmt The format string for the debug message, similar to printf.
 * @param ... Variadic arguments corresponding to the format specifiers in the
 * fmt parameter.
 */
#undef DPRINTF
#define DPRINTF(fmt, ...)                                               \
  do {                                                                  \
    const char *file =                                                  \
        strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__; \
    fprintf(stderr, "%s:%d:%s(): " fmt "", file, __LINE__, __func__,    \
            ##__VA_ARGS__);                                             \
  } while (0)
#undef DPRINTFRAW
#define DPRINTFRAW(fmt, ...)             \
  do {                                   \
    fprintf(stderr, fmt, ##__VA_ARGS__); \
  } while (0)

#else
#undef DPRINTF
#define DPRINTF(fmt, ...)
#undef DPRINTFRAW
#define DPRINTFRAW(fmt, ...)
#endif

#endif  // DEBUG_H
