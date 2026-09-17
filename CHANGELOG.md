# Changelog

## v1.1.0 (2026-09-17) — robustness

Everything here is a fix or a recovery path; there are no new features. The
release also changes how the firmware is built: **v1.1.0 ships a `Release`
build**, where every release up to `v1.0.1beta` shipped `MinSizeRel` because
`Release` did not survive on hardware.

### The Remote API could not complete an upload

Any upload much beyond 200 KB failed, and three separate bugs were stacked
behind it:

- **Every upload silently lost 437 bytes.** The body bytes that shared the
  first TCP segment with the request headers were dropped, and the server
  acknowledged them anyway, so the client had nothing to re-send and the
  upload could never finish.
- **Transfers were closed while still running.** The idle sweeper closed any
  connection a few seconds after it was accepted, because the poll timer it
  relied on is only reset by data the *server* sends — and during an upload
  the server sends nothing.
- **A 4 MB upload then hit the watchdog**, because extending a large file
  makes the card walk its cluster chain for longer than the 8 second limit.

A 4 MB upload and download now round-trip byte-identical, and a client
reading at 5 KB/s gets its file instead of being cut off after 8 seconds.

### Aborted transfers no longer wedge the API

A client that disappeared mid-transfer left the transfer lock held, so every
later upload or download answered `503 busy` until the device was reset. It
also left FatFs handles counted against a table shared with GEMDRIVE, and a
half-written file on the card.

### A pulled SD card comes back on its own

Putting a card back used to require a reset. Worse, with no card the device
did not say so: `volume` answered `200` with the size and free space it had
cached, and a directory listing answered `200` and empty.

Now the card is noticed within about two seconds and remounted by itself,
every endpoint that needs it answers `503 no_sd_card`, the setup menu shows
`SD: NO CARD`, and `[G]` and `[U]` refuse to start rather than launching a
mode with no drive behind it.

### The Atari no longer duplicates data on a retried write

When a write chunk timed out, the ST re-sent it and the firmware appended it
a second time: the file gained a duplicated block and lost its tail. Chunks
now carry a sequence number, so a re-sent chunk is recognised and answered
without being written again.

### Wi-Fi

- **The radio was always in power save**, whatever the setting said, because
  the driver re-applies its own default every time the interface comes up.
  It now runs at full power: round-trip latency improves from 99 ms to 27 ms
  average, and packet loss from 15% to 5% on the bench network.
- **The device rejoins by itself** after a link loss, instead of staying off
  the network until someone reset it.
- **A bad static IP configuration can no longer crash the boot.** A missing
  or malformed address used to fault before the setup menu appeared — the
  menu being the only place to fix it. Anything invalid now falls back to
  DHCP and says why on the menu.
- **The Wi-Fi password no longer appears in the debug log.** It was printed
  on every boot.

### Crashes are visible and survivable

A panic, a HardFault or an 8 second hang now reboots the device and reports
what happened on the menu's top line (`Recovered: hang in main_loop x2`),
with the reason, the phase and the faulting address also in
`GET /api/v1/system/health`. A crash-loop guard keeps the device in the menu
instead of re-entering whatever crashed. Core 0's stack moved to the top of
RAM with an MPU guard, so an overflow faults instead of quietly corrupting
memory.

### The CLI reports dropped connections

`sidecart.py` used to print a Python traceback when the device rebooted
under a command. It now prints one line and exits with the network status.

### Known limitations

- **Programs that trace heavily through the debug ABI can take the ST
  down.** A program reading more than a few thousand bytes through the
  cartridge debug window during `runner exec` faults the Atari; the rate
  rises with the size of the burst. The cause is not yet found — the
  firmware side has been measured and cleared — so heavy tracing should be
  kept to short bursts for now.
- Wi-Fi recovery is verified against injected faults, not against a real
  access-point outage.

## v1.0.1beta (2026-05-05) — stability fixes

Patch release. Recovery paths and visibility upgrades; no new
features beyond what's enumerated below. Drop-in upgrade from
`v1.0.0beta` via the Booster catalog.

### Fixes

- **SELECT button now works.** Short tap on the cartridge's
  physical SELECT button does a soft reset of the Pico
  (cartridge boots back into the setup menu); a long press
  (≥ 10 s) does a factory reset (erases the saved aconfig and
  reboots). On `v1.0.0beta` the button was wired but never
  reached a callback, so a press did nothing.

- **Bad relocation address no longer crashes the menu.** The
  GEMDRIVE blob installer was previously running at TOS init
  before the menu painted, so a misconfigured relocation
  address (e.g. one that overlapped the active stack) would
  crash the m68k before the user had a chance to see anything.
  The installer now runs at mode-commit time, after the menu
  has had a chance to paint, so `[R]eloc addr` is reachable
  for recovery. A new safety check additionally aborts the
  installer with a clear `Reloc/stack overlap.` banner when
  the chosen destination would land on or near the live
  supervisor stack.

- **`_phystop` tampering surfaces as `(!)`.** The setup-menu
  GEMDRIVE block now shows the read-only `_phystop` value
  (`$42E`). When TOS' phystop disagrees with the silicon's
  MMU bank-config nibble at `$FFFF8001` — a sign that a
  reset-resistant program lowered phystop and survived warm
  reset — a `(!)` marker appears next to the value. The cure
  is a power-cycle; the marker exists to make that
  conspicuous instead of leaving the user staring at "RAM
  shrank for no reason" symptoms.

- **`_v_bas_ad` (logical screen base) now visible.** New
  read-only `Screenmem` row in the same GEMDRIVE block,
  alongside the new Phystop row.

### Setup menu changes

- `[E]xit (launch)` renamed to **`[G]EMDRIVE`** and the key
  rebound from `E` to `G`. The "exit" framing was misleading
  — the verb commits GEMDRIVE-only mode (drops the ST into
  the emulated drive), not "leave the firmware". The new
  label is self-explanatory and frees up `[E]` for future
  use.

- `[F]` (firmware) alias removed. It was a duplicate of
  `[G]`/`[E]` that wasn't displayed on the bottom strip; the
  remaining mode-commit verbs are exactly `[G]` (GEMDRIVE),
  `[U]` (Runner), `[X]` (Booster).

- Hidden command-line entries (`m`, `?`, `print`, `save`,
  `erase`, `get`, `put_int`, `put_bool`, `put_str`) removed.
  None of these were displayed on the menu, but they were
  reachable via direct keyboard input — `save` / `erase` in
  particular let a stray keystroke mutate or wipe the
  aconfig flash sector silently. The visible menu is now
  exactly the reachable command set.

- **Default Advanced Runner hook is now `vbl ($70)`** (was
  `etv_timer ($400)` in `v1.0.0beta`). `vbl` is the only
  vector on which `runner adv jump` and `runner adv load`
  work, so making it the default means the full Advanced
  Runner surface is available without a setup-menu detour.
  The `[V]` toggle still flips between the two vectors.

- **Relocation region grew from 8 KB to a consolidated
  16 KB below the screen base.** Both the GEMDRIVE blob and
  the Runner blob now live inside `[screen_base − 16 KB,
  screen_base)` with growth headroom for both. The default
  reloc-addr label changed from `auto (screen-8KB)` to
  `auto (screen-16KB)`.

### Documentation

- README: dedicated `### SELECT button` subsection, new
  `### Stability banners` subsection covering the
  `Reloc/stack overlap` halt and the `Phystop … (!)` marker.

---

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
