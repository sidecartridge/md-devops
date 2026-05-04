# Changelog

## v1.0.0beta (2026-05-05) — first public release

First release. Install via the SidecarTridge Multi-device's
Booster app.

### Features

- **GEMDRIVE folder-as-drive** — mount a microSD subdirectory
  as a TOS drive letter (default `C: → /devops`).
- **Runner mode** — workstation-driven `Pexec` of any TOS /
  PRG, plus `cd`, `res`, `meminfo`, and `reset`.
- **`Pexec` load / exec / unload split** — load once, re-exec
  many times, free explicitly.
- **Advanced Runner** — VBL-ISR control surface that survives
  wedged programs (infinite loops, bombs, disabled traps).
  Reads memory, jumps execution, streams files into ST RAM.
  Hook vector switchable in the setup menu (`vbl ($70)`
  default; `etv_timer ($400)` opt-in).
- **Remote HTTP file management** — `volume`, `ls`, `get`
  (resume), `put` (overwrite), `rm`, `mv`, `mkdir`, `rmdir`,
  `mvdir`. 4 MB upload cap.
- **Fast debug traces** — one-cycle byte-emit ABI from m68k
  (`*(volatile char *)(0xFBFF00 + c)`) streamed over HTTP
  `tail -f` or USB CDC.
- **Live setup menu** — status icons, animated boot
  countdown, live USB CDC attach state.
- **`cli/sidecart.py`** — single-file stdlib-only Python CLI
  (3.10+, no `pip install`).

### Compatibility

ST, STE, MegaST, MegaSTE. Any TOS that boots a stock
cartridge ROM.

### Caveats

- HTTP API has **no authentication** — keep it on a trusted
  LAN.
- File names are FAT 8.3 (stem ≤ 8, ext ≤ 3, ASCII).
- `adv jump` / `adv load` need the `vbl ($70)` hook (the
  default); they return `409 wrong_hook` on `etv_timer`.
