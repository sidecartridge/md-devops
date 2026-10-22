/* SPDX-License-Identifier: GPL-3.0-only
 *
 * ROM4CHK.TOS — reads the rom4 area, the 1 KB of the cartridge window
 * that the workstation writes with `sidecart rom4 write`.
 *
 * Finds the area through the magic in shared-variable slot 20, shows
 * the first 32 bytes on screen for three seconds and sends the same
 * lines to the debug stream. Exits with 0 when it found the area and
 * 1 when the magic is missing (firmware without rom4, or no md-devops
 * cartridge).
 *
 * Build: ./build.sh (dist/ROM4CHK.TOS)
 *
 * Try it:
 *   python3 cli/sidecart.py rom4 write 0 52 4F 4D 34 20 4F 4B 21
 *   python3 cli/sidecart.py gemdrive put target/atarist/test/rom4/dist/ROM4CHK.TOS
 *   python3 cli/sidecart.py debug tail          # in another shell
 *   python3 cli/sidecart.py runner run /ROM4CHK.TOS
 *   python3 cli/sidecart.py runner status       # last : EXECUTE /ROM4CHK.TOS (exit=0)
 */

#include <mint/osbind.h>

/* The one fixed address: shared-variable slot 20, $FA2810 + 20 * 4.
 * Slot 21 holds the area's address and slot 22 its size. */
#define ROM4_SLOTS_ADDR 0xFA2860UL
#define ROM4_MAGIC 0x52344131UL /* 'R4A1' */
#define SHOW_BYTES 32

/* Any read of $FBFF00 + c sends byte c to the debug stream. */
#define DEBUG_BASE_ADDR 0xFBFF00UL

/* _hz_200 counts 200 a second whatever the screen's refresh rate. */
#define HZ200_ADDR 0x4BAL
#define HOLD_TICKS (3 * 200)

/* _hz_200 is below $800, so it is only readable in supervisor mode. */
static long read_hz200(void) { return *(volatile long *)HZ200_ADDR; }

static void debug_puts(const char *s) {
  while (*s) {
    (void)*(volatile char *)(DEBUG_BASE_ADDR + (unsigned char)*s++);
  }
}

/* Show a line on screen and send it to the debug stream. */
static void say(const char *s) {
  Cconws(s);
  Cconws("\r\n");
  debug_puts(s);
  debug_puts("\n");
}

static char *put_text(char *out, const char *s) {
  while (*s) {
    *out++ = *s++;
  }
  return out;
}

static char *put_hex(char *out, unsigned long value, int digits) {
  for (int i = digits - 1; i >= 0; i--) {
    out[i] = "0123456789ABCDEF"[value & 15];
    value >>= 4;
  }
  return out + digits;
}

/* Keep the lines on screen when started from the desktop. */
static void hold(void) {
  long start = Supexec(read_hz200);
  while (Supexec(read_hz200) - start < HOLD_TICKS) {
    Vsync();
  }
}

int main(void) {
  const volatile unsigned long *slots =
      (const volatile unsigned long *)ROM4_SLOTS_ADDR;
  char line[40];
  char *p;

  unsigned long magic = slots[0];
  if (magic != ROM4_MAGIC) {
    p = put_text(line, "rom4: no magic ($");
    p = put_hex(p, magic, 8);
    p = put_text(p, ")");
    *p = '\0';
    say(line);
    hold();
    return 1;
  }

  unsigned long address = slots[1];
  p = put_text(line, "rom4: $");
  p = put_hex(p, address, 6);
  p = put_text(p, ", $");
  p = put_hex(p, slots[2], 4);
  p = put_text(p, " bytes");
  *p = '\0';
  say(line);

  const volatile unsigned char *area =
      (const volatile unsigned char *)address;
  for (int row = 0; row < SHOW_BYTES; row += 8) {
    p = put_hex(line, (unsigned long)row, 2);
    *p++ = ':';
    for (int i = 0; i < 8; i++) {
      *p++ = ' ';
      p = put_hex(p, area[row + i], 2);
    }
    *p = '\0';
    say(line);
  }

  hold();
  return 0;
}
