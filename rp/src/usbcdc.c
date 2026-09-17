/**
 * File: usbcdc.c
 * Author: Diego Parrilla Santamaría
 * Date: May 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: USB CDC sink for the debugcap ring.
 */

#include "include/usbcdc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/debugcap.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#if !defined(DEVOPS_NO_USBCDC) || (DEVOPS_NO_USBCDC == 0)
// Only present when USB stdio is compiled in (EPIC-18 STORY-01).
#include "pico/stdio_usb.h"
#include "tusb.h"
#endif

static bool g_usbcdcInitialized = false;
static debugcap_cursor_t g_usbcdcCursor = {.read_pos = 0, .dropped = 0};
// Tracks the previous tud_cdc_connected() observation so the drain
// can detect the false→true rising edge when a host (re)attaches.
// Initial value `false` means a host that's already attached at
// boot still gets the snapshot on the first drain call — the same
// "bytes from connect time forward" UX as a late attach.
static bool g_usbcdcLastConnected = false;

void usbcdc_init(void) {
#if defined(DEVOPS_NO_USBCDC) && (DEVOPS_NO_USBCDC != 0)
  // Built without USB on purpose: never start TinyUSB (EPIC-18 STORY-01).
  return;
#else
  if (g_usbcdcInitialized) {
    return;
  }
  // Start USB stdio (TinyUSB and its background task) unless main.c already
  // did, through stdio_init_all() in a debug build. Not stdio_init_all()
  // again: it is not idempotent. It re-runs uart_init(), which resets the
  // UART and throws away up to 32 bytes still in the console FIFO, and
  // stdio_usb_init() claims another IRQ and background task on every call.
  if (!tud_inited()) {
    stdio_usb_init();
  }

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
#endif
}

void __not_in_flash_func(usbcdc_drain)(void) {
#if defined(DEVOPS_NO_USBCDC) && (DEVOPS_NO_USBCDC != 0)
  return;  // no USB in this build (EPIC-18 STORY-01)
#else
  if (!g_usbcdcInitialized) {
    return;
  }
  bool connected = tud_cdc_connected();
  if (connected && !g_usbcdcLastConnected) {
    // Rising edge: the host just attached (or reattached). Skip
    // the cursor to "now" so the workstation sees only bytes
    // emitted from this point forward (no stale tail of pre-
    // attach data) and fold the unread lag into cur->dropped so
    // the loss is still visible. The drain doesn't run while
    // disconnected — without this catch-up, drops accumulated
    // during the disconnect window would simply vanish.
    debugcap_cursor_skipToNow(&g_usbcdcCursor);
  }
  g_usbcdcLastConnected = connected;
  // No host on the other end → don't fill the TX FIFO. The
  // producer keeps writing the ring; the cursor lazily catches
  // up (per-cursor drop count) when a host eventually attaches.
  if (!connected) {
    return;
  }
  uint32_t avail = tud_cdc_write_available();
  if (avail == 0) {
    return;
  }
  // 512 B drain batch. Sized to absorb a single
  // HELLODBG-style chunk in one tud_cdc_write call; smaller than
  // CFG_TUD_CDC_TX_BUFSIZE (1024) so we don't try to push more
  // than the FIFO can hold and lose the tail.
  uint8_t buf[512];
  uint32_t want = (avail < sizeof(buf)) ? avail : (uint32_t)sizeof(buf);
  uint32_t take = debugcap_cursor_pull(&g_usbcdcCursor, buf, want);
  if (take == 0) {
    return;
  }
  tud_cdc_write(buf, take);
  tud_cdc_write_flush();
#endif
}

void usbcdc_getStats(uint32_t *dropped, bool *attached) {
  if (dropped != NULL) {
    *dropped = g_usbcdcInitialized ? g_usbcdcCursor.dropped : 0u;
  }
  if (attached != NULL) {
#if defined(DEVOPS_NO_USBCDC) && (DEVOPS_NO_USBCDC != 0)
    *attached = false;
#else
    *attached = g_usbcdcInitialized ? tud_cdc_connected() : false;
#endif
  }
}
