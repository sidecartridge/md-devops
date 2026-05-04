/**
 * File: debugcap.h
 * Author: Diego Parrilla Santamaría
 * Date: May 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Fast debug-byte capture (Epic 05 v2) — public API.
 *
 * Debug-byte ABI (m68k side, public): any read in the 256-byte
 * window $FBFF00..$FBFFFF latches the byte (A7..A0 of the
 * effective address) into the RP-side debug ring. Encoding:
 *
 *     #define DEBUG_BASE 0xFBFF00UL
 *     (void)*(volatile char *)(DEBUG_BASE + c);   // emit byte c
 *
 * Internally this rides on the existing ROM3 commemul capture
 * pipeline. The chandler ingest callback runs two consumers in
 * parallel on every 16-bit captured sample:
 *   1. The existing TPROTOCOL frame parser.
 *   2. This module's filter — `(sample & 0xFF00) == 0xFF00`
 *      AND firmware-mode is committed → emit `(sample & 0xFF)`.
 *
 * Pre-firmware-mode emits are dropped at the handler so menu-mode
 * activity doesn't pollute the diagnostic stream.
 *
 * Multi-consumer model (Epic 05 v2 / S4): the producer always
 * writes to the ring; consumers each keep their own
 * (read_pos, dropped) cursor and drain independently. A consumer
 * that lags by more than DEBUGCAP_RING_BYTES jumps its read_pos
 * forward to the oldest still-valid byte and increments its
 * per-cursor drop count.
 */

#ifndef DEBUGCAP_H
#define DEBUGCAP_H

#include <stdbool.h>
#include <stdint.h>

// Ring size — must be a power of 2 (the producer/consumer cursors
// wrap by mask, so size & (size-1) == 0 is required). 8 KB lets
// HELLODBG-style bursts (~14 KB) ride out a slow consumer for
// most of a burst before per-cursor drops kick in, and matches
// the commemul ring's order of magnitude so neither stage is the
// obvious bottleneck.
//
// Note: this lives in BSS as a plain static array (no alignment
// requirement, no DMA), so the size is purely a static-allocation
// cost — no linker rearrangement.
#define DEBUGCAP_RING_BYTES 8192

/**
 * @brief Per-consumer cursor over the debug ring. Each independent
 *        consumer (DPRINTF drainer, HTTP /api/v1/debug/log streamer,
 *        future USB CDC sink) holds one of these. The producer is
 *        oblivious to consumer count — it just writes.
 *
 *        Initialise with debugcap_cursor_initSnapshot to start
 *        at the "now" position (the consumer sees only bytes
 *        emitted from this point forward).
 */
typedef struct debugcap_cursor {
  uint32_t read_pos;  // bytes-since-boot; difference vs producer's
                      // write_pos is the cursor's pending data.
  uint32_t dropped;   // bytes lost because the producer wrapped
                      // past this cursor before it could read.
} debugcap_cursor_t;

/**
 * @brief Initialise a cursor at the current producer write
 *        position. The consumer will see only bytes emitted
 *        AFTER this call returns.
 */
void debugcap_cursor_initSnapshot(debugcap_cursor_t *cur);

/**
 * @brief Pull up to `max_bytes` from the cursor into `out`, in
 *        emit order. Advances the cursor. Returns the number of
 *        bytes actually copied (0 if no new data, or if `cur` /
 *        `out` are NULL). If the producer has wrapped past this
 *        cursor since the last call, the cursor jumps to the
 *        oldest still-valid byte and `cur->dropped` is
 *        incremented by the count of skipped bytes.
 */
uint32_t debugcap_cursor_pull(debugcap_cursor_t *cur, uint8_t *out,
                              uint32_t max_bytes);

/**
 * @brief Append one debug byte to the ring. Called by the
 *        chandler ingest filter on every captured sample whose
 *        high byte is 0xFF (and only when firmware mode is
 *        active). Cheap — single producer-side cursor advance,
 *        no locking.
 */
void debugcap_emit(uint8_t b);

/**
 * @brief Read out current ring statistics. All three fields may
 *        be NULL (skipped). The producer never drops — it just
 *        overwrites — so `dropped` here always reads 0 and is
 *        retained only for envelope-shape compatibility with the
 *        diagnostics endpoint. Per-cursor drops (the meaningful
 *        signal for a given consumer) are reported on the
 *        debugcap_cursor_t.dropped field instead.
 *
 *        `used` = min(total bytes emitted since boot, ring size)
 *        — i.e. how much of the ring currently holds valid data
 *        from the producer's perspective.
 *
 *        `capacity` = ring size.
 */
void debugcap_getRingStats(uint32_t *used, uint32_t *capacity,
                           uint32_t *dropped);

#endif  // DEBUGCAP_H
