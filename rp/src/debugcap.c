/**
 * File: debugcap.c
 * Author: Diego Parrilla Santamaría
 * Date: May 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Fast debug-byte capture (Epic 05 v2) — small
 *              multi-consumer ring + per-cursor pull API for
 *              HTTP / USB consumers.
 */

#include "include/debugcap.h"

#include <stddef.h>
#include <stdint.h>

// Single producer; consumers each carry their own cursor. Producer
// always writes to the ring (no producer-side drop decision); a
// consumer that has wrapped past the ring's last byte lazily
// catches up to the oldest still-valid byte on its next pull and
// increments its per-cursor drop count.
static uint8_t g_debugRing[DEBUGCAP_RING_BYTES];
static volatile uint32_t g_debugWritePos = 0;

#define DEBUGCAP_RING_MASK ((uint32_t)DEBUGCAP_RING_BYTES - 1u)

void debugcap_emit(uint8_t b) {
  g_debugRing[g_debugWritePos & DEBUGCAP_RING_MASK] = b;
  g_debugWritePos++;
}

void debugcap_cursor_initSnapshot(debugcap_cursor_t *cur) {
  if (cur == NULL) return;
  cur->read_pos = g_debugWritePos;
  cur->dropped = 0;
}

uint32_t debugcap_cursor_pull(debugcap_cursor_t *cur, uint8_t *out,
                              uint32_t max_bytes) {
  if (cur == NULL || out == NULL || max_bytes == 0) return 0;
  uint32_t write = g_debugWritePos;
  if (cur->read_pos == write) return 0;

  uint32_t lag = write - cur->read_pos;
  if (lag > (uint32_t)DEBUGCAP_RING_BYTES) {
    // Producer has wrapped past us; the older bytes are gone.
    // Catch up to the oldest still-valid byte.
    uint32_t lost = lag - (uint32_t)DEBUGCAP_RING_BYTES;
    cur->dropped += lost;
    cur->read_pos = write - (uint32_t)DEBUGCAP_RING_BYTES;
    lag = (uint32_t)DEBUGCAP_RING_BYTES;
  }

  uint32_t take = (lag < max_bytes) ? lag : max_bytes;
  for (uint32_t i = 0; i < take; i++) {
    out[i] = g_debugRing[(cur->read_pos + i) & DEBUGCAP_RING_MASK];
  }
  cur->read_pos += take;
  return take;
}

void debugcap_cursor_skipToNow(debugcap_cursor_t *cur) {
  if (cur == NULL) return;
  uint32_t write = g_debugWritePos;
  uint32_t lag = write - cur->read_pos;
  cur->dropped += lag;
  cur->read_pos = write;
}

void debugcap_getRingStats(uint32_t *used, uint32_t *capacity,
                           uint32_t *dropped) {
  uint32_t write = g_debugWritePos;
  uint32_t valid = (write > (uint32_t)DEBUGCAP_RING_BYTES)
                       ? (uint32_t)DEBUGCAP_RING_BYTES
                       : write;
  if (used != NULL) *used = valid;
  if (capacity != NULL) *capacity = (uint32_t)DEBUGCAP_RING_BYTES;
  // Producer never drops — it overwrites. Per-consumer drops are
  // reported on debugcap_cursor_t.dropped instead.
  if (dropped != NULL) *dropped = 0;
}
