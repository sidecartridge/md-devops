/**
 * File: rom4.c
 * Author: Neil Rackett
 * Date: October 2026
 * Copyright: 2026 - Neil Rackett
 * Description: The rom4 area: 1 KB of the cartridge window that the
 *              workstation writes over HTTP and any ST program reads.
 *              See rom4.h.
 */

#include "include/rom4.h"

#include <string.h>

#include "debug.h"
#include "memfunc.h"

extern unsigned char __rom_in_ram_start__[];

_Static_assert(ROM4_OFFSET + ROM4_SIZE <= CHANDLER_FRAMEBUFFER_OFFSET,
               "the rom4 area runs into the framebuffer");

// The RP keeps the window byte-pair swapped: the ST's byte i is the RP's
// byte i ^ 1, so the halfword at an even offset holds the ST's word.
static volatile uint16_t *area(void) {
  return (volatile uint16_t *)((uint32_t)&__rom_in_ram_start__ + ROM4_OFFSET);
}

void rom4_init(void) {
  uint32_t base = (uint32_t)&__rom_in_ram_start__;
  memset((void *)(base + ROM4_OFFSET), 0, ROM4_SIZE);
  // The magic last: a program that sees it can trust the other two.
  SET_SHARED_VAR(ROM4_SVAR_ADDRESS, rom4_getAddress(), base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(ROM4_SVAR_SIZE, ROM4_SIZE, base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(ROM4_SVAR_MAGIC, ROM4_MAGIC, base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

uint32_t rom4_getAddress(void) { return ROM4_CARTRIDGE_ADDR + ROM4_OFFSET; }

bool rom4_write(uint32_t offset, const uint8_t *src, uint32_t len) {
  if (offset > ROM4_SIZE || len > ROM4_SIZE - offset) {
    return false;
  }
  volatile uint16_t *words = area();
  uint32_t end = offset + len;
  uint32_t pos = offset;
  while (pos < end) {
    // One store per word, merging a lone byte at either edge with the
    // byte already there.
    uint32_t index = pos >> 1;
    uint16_t word = words[index];
    if ((pos & 1u) != 0) {
      word = (uint16_t)((word & 0xFF00u) | *src++);
      pos++;
    } else if (pos + 1 < end) {
      word = (uint16_t)((src[0] << 8) | src[1]);
      src += 2;
      pos += 2;
    } else {
      word = (uint16_t)((word & 0x00FFu) | (*src++ << 8));
      pos++;
    }
    words[index] = word;
  }
  return true;
}

void rom4_read(uint8_t *dst) {
  volatile uint16_t *words = area();
  for (uint32_t i = 0; i < ROM4_SIZE / 2; i++) {
    uint16_t word = words[i];
    dst[2 * i] = (uint8_t)(word >> 8);
    dst[2 * i + 1] = (uint8_t)word;
  }
}
