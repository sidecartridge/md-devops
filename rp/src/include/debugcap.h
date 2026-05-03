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
 */

#ifndef DEBUGCAP_H
#define DEBUGCAP_H

#include <stdbool.h>
#include <stdint.h>

// Ring size — must be a power of 2 (the producer/consumer cursors
// wrap by mask, so size & (size-1) == 0 is required). 256 absorbs
// HELLODBG.TOS's full 14000-byte burst with margin and lets a
// slow-drain consumer (DPRINTF on UART) catch up over a few
// iterations of the menu poll loop without dropping bytes.
//
// Note: this lives in BSS as a plain static array (no alignment
// requirement, no DMA), so the +3.8 KB vs the previous 256-byte
// ring is purely a static-allocation cost — no linker rearrangement.
#define DEBUGCAP_RING_BYTES 256

/**
 * @brief Append one debug byte to the ring. Called by the
 *        chandler ingest filter on every captured sample whose
 *        high byte is 0xFF (and only when firmware mode is
 *        active). Cheap — single producer-side cursor advance,
 *        no locking.
 */
void debugcap_emit(uint8_t b);

/**
 * @brief Drain any new bytes from the ring and emit them to the
 *        RP console via DPRINTF, then advance the consumer
 *        cursor. Intended to be called from a polling loop on
 *        Core 0 (the menu/idle loop in emul_start). Cheap when
 *        the ring is empty (just two-word comparison).
 */
void debugcap_drainToConsole(void);

/**
 * @brief Read out current ring statistics. All three fields may
 *        be NULL (skipped). `used` = bytes captured but not yet
 *        drained; `capacity` = ring size; `dropped` = cumulative
 *        count of bytes lost because the ring was full at the
 *        moment of an emit.
 */
void debugcap_getRingStats(uint32_t *used, uint32_t *capacity,
                           uint32_t *dropped);

#endif  // DEBUGCAP_H
