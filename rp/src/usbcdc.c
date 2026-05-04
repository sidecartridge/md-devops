/**
 * File: usbcdc.c
 * Author: Diego Parrilla Santamaría
 * Date: May 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Epic 05 v2 / S5 — USB CDC sink for the debugcap ring.
 */

#include "include/usbcdc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/debugcap.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

static bool g_usbcdcInitialized = false;
static debugcap_cursor_t g_usbcdcCursor = {.read_pos = 0, .dropped = 0};

void usbcdc_init(void) {
  if (g_usbcdcInitialized) {
    return;
  }
  // Idempotent in the pico-sdk — fine to call even if main.c
  // already invoked it under _DEBUG=1.
  stdio_init_all();

  // Detach the CDC interface from stdio so fprintf(stderr, ...)
  // (DPRINTF) only lands on UART (when stdio_uart is enabled in
  // _DEBUG=1) and never on the USB CDC port. TinyUSB stays alive
  // — only the stdio→CDC bridge is removed, leaving the CDC
  // interface fully under our control via tud_cdc_write.
  stdio_set_driver_enabled(&stdio_usb, false);

  // Snapshot the cursor at "now" so a freshly-opened terminal
  // sees only bytes emitted from this point forward, matching
  // the HTTP tail's behaviour.
  debugcap_cursor_initSnapshot(&g_usbcdcCursor);

  g_usbcdcInitialized = true;
}

void __not_in_flash_func(usbcdc_drain)(void) {
  if (!g_usbcdcInitialized) {
    return;
  }
  // No host on the other end → don't fill the TX FIFO. The
  // producer keeps writing the ring; the cursor lazily catches
  // up (per-cursor drop count) when a host eventually attaches.
  if (!tud_cdc_connected()) {
    return;
  }
  uint32_t avail = tud_cdc_write_available();
  if (avail == 0) {
    return;
  }
  uint8_t buf[64];
  uint32_t want = (avail < sizeof(buf)) ? avail : (uint32_t)sizeof(buf);
  uint32_t take = debugcap_cursor_pull(&g_usbcdcCursor, buf, want);
  if (take == 0) {
    return;
  }
  tud_cdc_write(buf, take);
  tud_cdc_write_flush();
}

void usbcdc_getStats(uint32_t *dropped, bool *attached) {
  if (dropped != NULL) {
    *dropped = g_usbcdcInitialized ? g_usbcdcCursor.dropped : 0u;
  }
  if (attached != NULL) {
    *attached = g_usbcdcInitialized ? tud_cdc_connected() : false;
  }
}
