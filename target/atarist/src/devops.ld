/* devops.ld — absolute layout for the cartridge image
 *
 * The cartridge image is loaded verbatim at $FA0000 by the cartridge
 * port hardware, so the offsets below become runtime addresses by
 * adding ROM4_ADDR. The 10 KB cartridge code budget
 * (CARTRIDGE_CODE_SIZE = $2800) is split:
 *
 *   $0000..$07FF  main.s     2 KB   boot, dispatch, terminal
 *   $0800..$1BFF  gemdrive.s 5 KB   GEMDOS hooks + protocol blob
 *                                   (relocated to RAM at boot)
 *   $1C00..$27FF  runner.s   3 KB   Runner foreground loop
 *                                   (self-relocates to RAM at runner_entry)
 *
 * The gemdrive blob is copied to a configurable RAM address
 * (default screen_base - 16 KB) by main.s's gemdrive_install once
 * the user commits a mode at the setup menu. The Runner
 * blob self-relocates at runner_entry to gemdrive_reloc +
 * RUNNER_ABOVE_GEMDRIVE_OFFSET (above gemdrive in the same 16 KB
 * protected region). Both .s files must be position-independent —
 * see the warnings at the top of gemdrive.s and runner.s.
 *
 * Adding a new module: add a ".text_<name> 0x????? : { <name>.o(.text) }"
 * section here, mirror the offset with `equ (ROM4_ADDR + $????)` in
 * main.s, add the .o target to target/atarist/Makefile, and account
 * for it in the cartridge code budget.
 */
SECTIONS
{
    .text_main 0x000000 : {
        main.o(.text)
    }

    .text_gemdrive 0x000800 : {
        gemdrive.o(.text)
    }

    .text_runner 0x001C00 : {
        runner.o(.text)
    }
}
