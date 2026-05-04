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

#endif  // USBCDC_H
