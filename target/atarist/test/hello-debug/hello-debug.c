/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Epic 05 v2 / S2 — HELLODBG.TOS
 *
 * Test program for the SidecarTridge md-devops fast-debug-traces
 * public ABI. Writes "Hello, world!\n" through the ROM3
 * debug-capture window of the cartridge.
 *
 * ABI contract (Epic 05 v2): any read in $FBFF00..$FBFFFF latches
 * the low byte (A7..A0 of the effective address) into the RP-side
 * debug ring. The 8-bit read result is undefined — debug
 * consumers MUST discard it.
 *
 * The window is only live in firmware mode (after the user has
 * committed [U]/[E]/[F] in the setup menu). Reads in menu mode
 * are captured by the same PIO state machine but discarded by the
 * RP-side filter, so they cause no harm.
 *
 * Build (m68k-atari-mint-gcc + libcmini inside the sidecartridge
 * atarist toolkit docker — see https://github.com/sidecartridge/
 * atarist-toolkit-docker):
 *
 *   ./build.sh
 *
 * Output: dist/HELLODBG.TOS (8.3 short name — Atari ST / GEMDOS
 * rejects long filenames on cartridge / drive listings).
 *
 * Try it:
 *   python3 cli/sidecart.py put dist/HELLODBG.TOS /
 *   python3 cli/sidecart.py runner run /HELLODBG.TOS
 *   # → RP debug console shows debugcap[14]: 48 65 6C 6C ... | "Hello, world!."
 */

#include <mint/osbind.h>

#define DEBUG_BASE_ADDR 0xFBFF00UL

/* Emit one byte through the debug window.
 *
 * Encoding: the address bus carries the byte on A7..A0 (the low
 * 8 bits of the effective address). The discriminator on the RP
 * side is "high byte == 0xFF" — i.e. address $FBFFxx where xx is
 * the byte being emitted. So debug_putc(c) reads at
 * DEBUG_BASE_ADDR + c, which generates the cartridge cycle the
 * RP-side PIO captures. The 8-bit read result is undefined and
 * discarded. */
static void debug_putc(unsigned char c) {
  (void)*(volatile char *)(DEBUG_BASE_ADDR + c);
}

static void debug_puts(const char *s) {
  while (*s) {
    debug_putc((unsigned char)*s++);
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  /* Visible confirmation on the Atari ST screen — Cconws is GEMDOS
   * function 9 (write NUL-terminated string to console). If this
   * prints but the RP debug console shows zero new bytes
   * afterwards, we know main() ran and the issue is specific to
   * the volatile-load encoding path below. */
  Cconws("HELLODBG: Hello, world!\r\n");

  /* Drive the public-ABI debug emit 1000 times — 14 chars per
   * pass = 14000 cartridge cycles. At ~8 MHz that's a ~2 ms burst.
   * The DPRINTF drain on the RP side should unmissably show the
   * "Hello, world!" pattern repeating. */
  for (int i = 0; i < 1000; i++) {
    debug_puts("Hello, world!\n");
  }

  return 0;
}
