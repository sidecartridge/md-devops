/**
 * File: sd_timeouts.c
 * Author: Diego Parrilla Santamaría
 * Date: September 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: SD driver timeouts, overriding fatfs-sdk's weak table.
 *
 *              The watchdog (health.c) reboots the RP when the main loop
 *              is not fed for 8 s. fatfs-sdk's defaults allow 2 s per
 *              card-ready wait with an 8 s card lock, so a pulled or
 *              failing card could block one FatFs call past it. These
 *              values keep a single SD operation to a few seconds. The
 *              locks are never contended here (one thread), so their
 *              timeouts only bound a bug.
 */

#include "sd_timeouts.h"

sd_timeouts_t sd_timeouts = {
    .sd_command = 1000,       // card-ready wait and data-token wait, ms
    .sd_command_retries = 3,  // resends when a command gets no response
    .sd_lock = 4000,
    .sd_spi_read = 1000,
    .sd_spi_write = 1000,
    .sd_spi_write_read = 1000,
    .spi_lock = 2000,
    // SDIO is not used on this board; fatfs-sdk's defaults.
    .rp2040_sdio_command_R1 = 10,
    .rp2040_sdio_command_R2 = 2,
    .rp2040_sdio_command_R3 = 2,
    .rp2040_sdio_rx_poll = 1000,
    .rp2040_sdio_tx_poll = 5000,
    .sd_sdio_begin = 1000,
    .sd_sdio_stopTransmission = 200,
};
