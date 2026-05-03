/**
 * File: debugcap.c
 * Author: Diego Parrilla Santamaría
 * Date: May 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Fast debug-byte capture (Epic 05 v2) — small ring
 *              + DPRINTF drain. Producer is the chandler ingest
 *              filter; consumer (in v2 / S1) is a DPRINTF drain
 *              called from the main poll loop. Future stories
 *              add USB CDC + HTTP consumers.
 */

#include "include/debugcap.h"

#include <stdint.h>
#include <stdio.h>

#include "debug.h"

// Single-producer single-consumer ring. The producer is
// chandler_consume_rom3_sample (running on Core 0 in the lwIP
// poll path); the consumer is debugcap_drainToConsole (also on
// Core 0). Both see consistent state because they run in the
// same loop iteration without preemption.
static uint8_t g_debugRing[DEBUGCAP_RING_BYTES];
static volatile uint32_t g_debugWritePos = 0;
static volatile uint32_t g_debugReadPos = 0;
static volatile uint32_t g_debugDropped = 0;

#define DEBUGCAP_DRAIN_BATCH 32u

static inline uint32_t debugcap_used(void) {
  return g_debugWritePos - g_debugReadPos;
}

void debugcap_emit(uint8_t b) {
  if (debugcap_used() >= (uint32_t)DEBUGCAP_RING_BYTES) {
    // Ring full — bytes the consumer hasn't drained yet would
    // get clobbered by an unbounded write. Increment the drop
    // counter and discard. (For high-rate emits, a future story
    // can grow the ring or drain on a faster cadence.)
    g_debugDropped++;
    return;
  }
  g_debugRing[g_debugWritePos & ((uint32_t)DEBUGCAP_RING_BYTES - 1u)] = b;
  g_debugWritePos++;
}

void debugcap_drainToConsole(void) {
  uint32_t used = debugcap_used();
  if (used == 0) return;

  // Cap this drain at DEBUGCAP_DRAIN_BATCH so a single DPRINTF
  // line stays small and the calling loop can keep up. Subsequent
  // calls drain the rest.
  uint32_t batch = (used > DEBUGCAP_DRAIN_BATCH) ? DEBUGCAP_DRAIN_BATCH : used;

  // Format: hex + ASCII rendering, single DPRINTF line. Mirror
  // shape from md-debug-cart's debug output:
  //   debugcap[N]: 48 65 6C 6C 6F 2C 20 ... | "Hello, ..."
  static const char hex_chars[] = "0123456789ABCDEF";
  char hexbuf[3 * DEBUGCAP_DRAIN_BATCH + 4];
  char asciibuf[DEBUGCAP_DRAIN_BATCH + 4];
  size_t hcap = sizeof(hexbuf);
  char *hp = hexbuf;
  char *ap = asciibuf;
  for (uint32_t i = 0; i < batch; i++) {
    uint32_t pos =
        (g_debugReadPos + i) & ((uint32_t)DEBUGCAP_RING_BYTES - 1u);
    uint8_t b = g_debugRing[pos];
    int n = snprintf(hp, hcap, "%02X ", (unsigned)b);
    if (n < 0 || (size_t)n >= hcap) break;
    hp += n;
    hcap -= (size_t)n;
    *ap++ = (b >= 32 && b < 127) ? (char)b : '.';
  }
  *ap = '\0';
  DPRINTF("debugcap[%lu]: %s| \"%s\"\n",
          (unsigned long)batch, hexbuf, asciibuf);

  g_debugReadPos += batch;
}

void debugcap_getRingStats(uint32_t *used, uint32_t *capacity,
                           uint32_t *dropped) {
  if (used != NULL) *used = debugcap_used();
  if (capacity != NULL) *capacity = (uint32_t)DEBUGCAP_RING_BYTES;
  if (dropped != NULL) *dropped = g_debugDropped;
}
