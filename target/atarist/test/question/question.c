/* SPDX-License-Identifier: GPL-3.0-only
 *
 * QUESTION.TOS — a known-answer test program for the Runner.
 *
 * Shows a line of text, waits three seconds, writes ANSWER.TXT
 * containing "42" to its current directory and exits with code 42,
 * so a run can be checked from the workstation without looking at
 * the ST: `runner status` shows the exit code and `gemdrive get`
 * fetches the file. If the file cannot be written it says so on
 * screen and exits with the GEMDOS error instead.
 *
 * Build: ./build.sh (dist/QUESTION.TOS)
 *
 * Try it:
 *   python3 cli/sidecart.py gemdrive put target/atarist/test/question/dist/QUESTION.TOS
 *   python3 cli/sidecart.py runner run /QUESTION.TOS
 *   python3 cli/sidecart.py runner status      # last : EXECUTE /QUESTION.TOS (exit=42)
 *   python3 cli/sidecart.py gemdrive get /ANSWER.TXT
 */

#include <mint/osbind.h>

/* _hz_200 counts 200 a second whatever the screen's refresh rate. */
#define HZ200_ADDR 0x4BAL
#define HOLD_TICKS (3 * 200)

/* _hz_200 is below $800, so it is only readable in supervisor mode. */
static long read_hz200(void) { return *(volatile long *)HZ200_ADDR; }

int main(void) {
  /* Two lines: low resolution has 40 columns and TOS does not wrap. */
  Cconws("What is the answer to the\r\nultimate question?\r\n");

  long start = Supexec(read_hz200);
  while (Supexec(read_hz200) - start < HOLD_TICKS) {
    Vsync();
  }

  long handle = Fcreate("ANSWER.TXT", 0);
  if (handle < 0) {
    Cconws("Could not create ANSWER.TXT\r\n");
    return (int)handle;
  }
  long written = Fwrite((short)handle, 2, "42");
  Fclose((short)handle);
  if (written != 2) {
    Cconws("Could not write ANSWER.TXT\r\n");
    return (written < 0) ? (int)written : -1;
  }

  return 42;
}
