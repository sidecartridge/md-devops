/**
 * File: rom4.h
 * Author: Neil Rackett
 * Date: October 2026
 * Copyright: 2026 - Neil Rackett
 * Description: The rom4 area: 1 KB of the cartridge window that the
 *              workstation writes over HTTP and any ST program reads.
 *              DevOps gives the bytes no meaning.
 *
 *              Programs find the area through three shared variables,
 *              so the only fixed address is the magic's slot,
 *              $FA2810 + 4 * ROM4_SVAR_MAGIC = $FA2860:
 *
 *                slot 20  ROM4_MAGIC, published at RP boot
 *                slot 21  ST address of the area
 *                slot 22  size in bytes
 *
 *              Zeroed at RP boot and written only by the HTTP API, so
 *              the contents survive an ST reset but not an RP reset.
 *              Each 16-bit word lands in one store, so the ST never
 *              reads half a word; a longer write can be seen part done.
 */

#ifndef ROM4_H
#define ROM4_H

#include <stdbool.h>
#include <stdint.h>

#include "chandler.h"

// ST address of the cartridge window (ROM4_ADDR in main.s).
#define ROM4_CARTRIDGE_ADDR 0x00FA0000u

// Inside APP_FREE, after the Runner's adv load staging buffer
// (APP_FREE + 0x4000..0x5FFF): $FA8B00..$FA8EFF.
#define ROM4_OFFSET (CHANDLER_APP_FREE_OFFSET + 0x6000)
#define ROM4_SIZE 1024u

#define ROM4_SVAR_MAGIC 20
#define ROM4_SVAR_ADDRESS 21
#define ROM4_SVAR_SIZE 22

#define ROM4_MAGIC 0x52344131u  // 'R4A1'

// Zero the area and publish its shared variables. Call once at boot,
// after the cartridge image is in RAM.
void rom4_init(void);

// ST address of the first byte of the area.
uint32_t rom4_getAddress(void);

// Copy `len` bytes, in ST order, to `offset` in the area. False, with
// nothing written, when the range does not fit.
bool rom4_write(uint32_t offset, const uint8_t *src, uint32_t len);

// Copy the whole area, ROM4_SIZE bytes in ST order, to `dst`.
void rom4_read(uint8_t *dst);

#endif  // ROM4_H
