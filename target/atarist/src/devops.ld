/* devops.ld — absolute layout for the cartridge image
 *
 * The cartridge image is loaded verbatim at $FA0000 by the cartridge
 * port hardware, so the offsets below become runtime addresses by
 * adding ROM4_ADDR. main.s gets the first 2 KB ($0000..$07FF) and
 * gemdrive.s occupies the next 6 KB ($0800..$1FFF); together they must
 * fit in CARTRIDGE_CODE_SIZE = 8 KB (the build script asserts this).
 *
 * The gemdrive blob is also copied at boot to a configurable RAM
 * address (default screen_base - 8 KB) by main.s's gemdrive_init, so
 * gemdrive.s must be position-independent — see the warning at the
 * top of gemdrive.s.
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
}
