/**
 * File: usbcdc.h
 * Author: Diego Parrilla Santamaría
 * Date: May 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Epic 05 v2 / S5 — USB CDC sink for the debugcap ring.
 *
 *              Reuses the pico-sdk's `pico_stdio_usb` plumbing to
 *              bring up TinyUSB + a single CDC interface, then
 *              detaches it from stdio (`stdio_set_driver_enabled`)
 *              so DPRINTF stays UART-only. Debug bytes are written
 *              raw via `tud_cdc_write` — byte-exact, no \r/\n
 *              translation, mirroring the HTTP /api/v1/debug/log
 *              octet-stream contract.
 */

#ifndef USBCDC_H
#define USBCDC_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialise stdio (idempotent), bring up TinyUSB / CDC,
 *        detach the CDC interface from stdio so DPRINTF doesn't
 *        bleed onto the wire, and snapshot a debugcap cursor at
 *        the current producer position. Safe to call from the
 *        emul_start path; no-op on a second call.
 */
void usbcdc_init(void);

/**
 * @brief Pump as many debug bytes as the host's TX FIFO can
 *        accept right now from the debugcap ring out the USB CDC
 *        interface. Cheap when no host is connected
 *        (tud_cdc_connected() short-circuits) and when the ring
 *        is empty. Intended to be called from the menu/idle loop
 *        on Core 0.
 */
void usbcdc_drain(void);

/**
 * @brief Snapshot the USB CDC sink's runtime stats. Either field
 *        may be NULL (skipped). `dropped` = the per-cursor count
 *        of debug bytes that fell off the back of the debugcap
 *        ring before this consumer could read them (i.e. the
 *        producer wrapped past the cursor while the host's TX
 *        FIFO stalled). `attached` = `tud_cdc_connected()` — true
 *        iff a host has the CDC port open with DTR asserted.
 *
 *        Cumulative since boot — includes disconnect-window
 *        loss (S7 folds the unread lag into `dropped` on every
 *        host (re)attach, so bursts that arrived while no one
 *        was listening are still counted) and any in-session
 *        drops where the host's TX FIFO stalled long enough
 *        for the producer to wrap. Pre-init callers see
 *        attached=false, dropped=0.
 */
void usbcdc_getStats(uint32_t *dropped, bool *attached);

#endif  // USBCDC_H
