# SidecarTridge Multi-device Microfirmware App template

This is the template to create a Microfirmware app for the SidecarTridge Multidevice-app for Atari ST computers.

# ⚠️ ATTENTION! READ THIS FIRST

The process for creating a microfirmware app from this template is now documented in the official [SidecarTridge Multi-device documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/). To avoid inconsistencies and outdated information, we've centralized the instructions there. Please refer to the official documentation for the latest guidance.

## Shared 64 KB region layout

The template now ships with a single source-of-truth layout for the 64 KB shared region (m68k `$FA0000`–`$FAFFFF`, mirrored at RP `0x20030000`):

- The cartridge image (m68k header + code) lives in the first **10 KB** (`$FA0000`–`$FA27FF`). `target/atarist/build.sh` enforces this with a hard size check on `BOOT.BIN`.
- A small fixed-offset metadata block (`CMD_MAGIC_SENTINEL`, `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, 60 × 4-byte indexed shared variables) sits at `$FA2800`.
- The **APP_FREE** arena (~46 KB at `$FA2B00`) is the contiguous space your app should use for its own buffers.
- The **framebuffer** (8000 B for 320×200 monochrome) sits at the very top of the region (`$FAE0C0`), so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block.

Both sides derive every offset symbolically from the constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). Apps must never hard-code an address inside the region — always reference the named offset/symbol so the layout stays the single source of truth.

See `programming.md` for the full table and the budget rules.

## Cartridge code layout

The cartridge image is split via `target/atarist/src/devops.ld` into three sections:

- `main.s` at offset `0x0000` (`$FA0000`, 2 KB) — boot, dispatch, terminal.
- `gemdrive.s` at offset `0x0800` (`$FA0800`, 5 KB) — GEMDOS hooks + protocol blob (relocated to RAM at boot).
- `runner.s` at offset `0x1C00` (`$FA1C00`, 3 KB) — Runner foreground loop (Epic 03).

`main.s`'s `check_commands` dispatch reads the cartridge sentinel and hands control to the right blob: `CMD_START = 4` jumps into `GEMDRIVE_BLOB+4` (diagnostic + memtop verify), and `CMD_START_RUNNER = 5` jumps into `RUNNER_BLOB` (the Runner's poll loop). Adding a new module follows the same pattern: place a new `.text_<name>` section in `devops.ld`, mirror the offset with an `equ` in `main.s`, and add the `.o` target to `target/atarist/Makefile`.

Both `gemdrive.s` and `runner.s` are 100 % relocatable and self-contained — they include `inc/sidecart_macros.s` and `inc/sidecart_functions.s` privately so the protocol `bsr`'s resolve locally inside their own object files. No `xref` / `xdef` cross-module references; no `jsr` / `jmp` to outside-module symbols (except the entry-point `jmp` from `main.s`'s dispatch). See `CLAUDE.md` for the full editing guardrails.

## Runner mode

Runner mode is the foreground execution path the firmware ships with. The user picks `[U]` at the setup terminal to launch it; the m68k Runner stays in a poll loop waiting for commands from the RP, while GEMDRIVE keeps servicing TOS file I/O so launched programs can use the emulated drive normally.

The Runner exposes its own subset of the HTTP API (`/api/v1/runner/*`) and CLI (`sidecart runner …`) for cold-resetting the ST, executing programs, navigating the cwd, switching screen resolutions, and reading a live system-memory snapshot. See [`docs/api.md`](docs/api.md) for the full reference (curl + sidecart examples for every endpoint).

```sh
python3 cli/sidecart.py runner status
python3 cli/sidecart.py runner cd /GAMES/ARKANOID
python3 cli/sidecart.py runner run RUNME.TOS
python3 cli/sidecart.py runner reset
python3 cli/sidecart.py runner res low
python3 cli/sidecart.py runner meminfo
```

### Advanced Runner

A second command surface (Epic 04) runs from inside the m68k's VBL ISR (`$70`, or `$400` if you switched `ADV_HOOK_VECTOR` to `etv_timer`), so it keeps working when a launched program has wedged the foreground poll loop — infinite loops, bombs already painted, traps disabled. Two of the three POSTs (`adv jump`, `adv load`) require the VBL hook specifically; `adv meminfo` works on either vector. None gate on the busy lock.

```sh
python3 cli/sidecart.py runner adv status                              # is the hook installed? which vector?
python3 cli/sidecart.py runner adv meminfo                             # snapshot from inside the ISR
python3 cli/sidecart.py runner adv jump 0xFA1C00                       # rte to a user address
python3 cli/sidecart.py runner adv load ./kernel.bin 0x40000           # stream a file into RAM
```

`runner adv jump` and `runner adv load` accept addresses in decimal, `0xhex`, or `$hex` — quote `$hex` in your shell (`'$FA1C00'`) or the shell will eat it as a variable reference. Prefer `0xhex` in scripts.

## Remote HTTP Management API

Once the device joins Wi-Fi it advertises an HTTP/1.1 REST API on
port 80 at `http://sidecart.local/` (mDNS hostname is the gconfig
`PARAM_HOSTNAME`, default `sidecart`). The API lets you list, upload,
download, rename, and delete files inside the GEMDRIVE folder (the
`GEMDRIVE_FOLDER` aconfig param, default `/devops`) from any
workstation on the same LAN.

A single-file, stdlib-only Python CLI (`cli/sidecart.py`) wraps the
endpoints with Unix-style verbs (`ls`, `get`, `put`, `rm`, `mv`,
`mkdir`, `rmdir`, `mvdir`, `volume`, `ping`).

```sh
python3 cli/sidecart.py ls /
python3 cli/sidecart.py put LOCAL.PRG -f
python3 cli/sidecart.py get GAME.TOS -r
```

The full endpoint reference (curl + sidecart examples for every
verb, error envelope, status codes) lives in
[`docs/api.md`](docs/api.md). The API has **no authentication** —
treat the network it's reachable on as trusted; don't expose it past
your LAN.

## Debug traces

The firmware exposes a lightweight debug-byte capture surface
that the Atari ST (and any TOS / PRG it runs) can write to with
a single cartridge cycle per byte, and that workstations can
consume over either HTTP or USB. Public m68k ABI: every read in
`$FBFF00..$FBFFFF` latches the low address byte (A7..A0) into
the debug ring; the 8-bit data result is undefined and MUST be
discarded.

> **Power-up ordering when debugging via USB.** If the Pico is
> powered through its own USB cable (e.g. plugged into your
> workstation for `sidecart debug tail` over CDC, or for serial
> DPRINTF), it boots *before* the Atari ST. Two side effects:
> 1. The setup-menu countdown starts running with no ST attached,
>    and once it elapses (~20 s by default) the firmware
>    auto-commits Runner mode (per Epic 06 / S2). When you
>    subsequently power the ST, it sees the cartridge already
>    in Runner state and skips the menu — you never get a
>    chance to press `[U]` / `[E]` / `[F]`.
> 2. Any Runner state the Pico accumulated before the ST booted
>    (a previously-loaded program, a stale cwd, etc.) survives
>    into the new ST session — the Pico has no way to detect
>    a fresh ST cold reset until GEMDRIVE HELLO arrives.
>
> If you want the menu countdown to run while you watch it,
> press a key on the ST keyboard before the timer expires
> (any key halts the countdown), or power the cartridge slot
> first and add the USB cable after the ST is up. Stopping the
> countdown also lets you observe the menu live-state (USB CDC
> attached/disconnected line, etc.) before committing to a
> mode.

**Emit one byte, C**:

```c
#define DEBUG_BASE 0xFBFF00UL
(void)*(volatile char *)(DEBUG_BASE + c);   // emit byte c
```

**Emit one byte, m68k assembly** (Motorola syntax — `vasm` /
`stcmd`):

```asm
DEBUG_BASE  equ $FBFF00

; In:  d0.b = byte to emit
; Out: -
; Trashes: d1, a0
            lea     DEBUG_BASE,a0
            moveq   #0,d1              ; zero-extend the byte → d1.w
            move.b  d0,d1
            tst.b   (a0,d1.w)          ; emit; read result discarded
```

**Dump a NUL-terminated string, C**:

```c
#define DEBUG_BASE 0xFBFF00UL

static void debug_putc(unsigned char c) {
  (void)*(volatile char *)(DEBUG_BASE + c);
}
static void debug_puts(const char *s) {
  while (*s) debug_putc((unsigned char)*s++);
}

debug_puts("Hello, world!\n");
```

**Dump a NUL-terminated string, m68k assembly**:

```asm
DEBUG_BASE  equ $FBFF00

; In:  a1 = pointer to NUL-terminated string
; Trashes: d0, a0
debug_puts:
            lea     DEBUG_BASE,a0
.loop:      moveq   #0,d0              ; zero-extend so d0.w is clean
            move.b  (a1)+,d0           ; load byte; sets Z if NUL
            beq.s   .done
            tst.b   (a0,d0.w)          ; emit
            bra.s   .loop
.done:      rts

; Call site:
            lea     msg,a1
            bsr     debug_puts
            ...

msg:        dc.b    'Hello, world!',$0A,0
            even
```

Two transports run side-by-side; pick whichever fits your dev
setup, or use both at once:

```sh
# Watch the byte stream over the network (no USB cable needed):
python3 cli/sidecart.py debug tail

# Or plug in a USB cable to the Pico and use any serial terminal:
screen /dev/tty.usbmodem*  115200          # macOS — baud is cosmetic

# Diagnostic envelope (firmware mode flag, ring stats, USB drop count):
python3 cli/sidecart.py debug status
```

A tiny m68k test program at `target/atarist/test/hello-debug/`
verifies the path end-to-end:

```sh
target/atarist/test/hello-debug/build.sh
python3 cli/sidecart.py put target/atarist/test/hello-debug/dist/HELLODBG.TOS /
python3 cli/sidecart.py runner run /HELLODBG.TOS
# → "Hello, world!\n" × 1000 appears on whichever transport you're watching.
```

The capture is gated on firmware-mode commit (`[U]` / `[E]` /
`[F]` in the menu) so menu activity never pollutes the stream.
Full endpoint reference + multi-consumer model is in
[`docs/api.md`](docs/api.md#debug-traces).

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file. 
