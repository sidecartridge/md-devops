# Changelog

## v1.1.0 (2026-09-17) — robustness

No new features. Things that used to fail now work, and when something does
go wrong the cartridge recovers by itself instead of waiting for you to
unplug it.

### Copying files over Wi-Fi, whatever the size

Anything much beyond 200 KB never arrived. A 4 MB file now goes up and comes
back down identical, and a slow connection is no longer cut off part way
through.

### A transfer that dies no longer blocks the next one

If the other end vanished mid-transfer — laptop asleep, cable out, Ctrl-C —
every later upload or download answered *busy* until the cartridge was
reset, and a half-written file was left on the card. Transfers now clean up
after themselves.

### The SD card

- **Take the card out and put it back**: it is picked up again in about two
  seconds. No reset needed.
- **With no card in**, the cartridge says so instead of pretending. The setup
  menu shows `SD: NO CARD`, `[G]` and `[U]` refuse to start, and the API
  answers *no SD card*. Before, it would report the last card's free space
  and show you an empty, cheerful, non-existent directory.

### Files written from the Atari arrive whole

When a write took too long and the Atari sent the same block again, that
block was written twice and the end of the file was lost. Retried blocks are
now recognised and written once.

### Wi-Fi

- **Faster and steadier.** The radio was always in power-saving mode,
  whatever the setting said. On the bench network, response time dropped
  from 99 ms to 27 ms and lost packets from 15% to 5%.
- **It rejoins on its own** after the router reboots or you walk out of
  range. Before, it stayed off the network until someone reset the
  cartridge.
- **A wrong fixed IP address no longer locks you out.** It used to crash
  before the setup menu appeared — the one place where you could correct it.
  Anything invalid now falls back to DHCP and tells you why on the menu.
- **Your Wi-Fi password is no longer printed** on the debug console at every
  boot.

### It recovers from its own crashes

If the firmware crashes or freezes, the cartridge reboots and tells you on
the top line of the menu — for example `Recovered: hang in main_loop x2` —
rather than sitting there dead. It will not fall back into the same crash
over and over: after a repeat it stays in the menu, where you can change
whatever caused it.

### The command-line tool

`sidecart.py` printed a page of Python errors when the cartridge rebooted
under a command. It now prints one line and stops.

### Known limitations

- **A program that traces heavily while running under the Runner can crash
  the Atari.** Reading more than a few thousand bytes through the cartridge
  debug window during `runner exec` bombs the machine, more often the bigger
  the burst. We have not found the cause yet, so keep tracing to short
  bursts for now.
- Wi-Fi recovery has been tested with faults we injected ourselves, not
  against a real access point going down.

### For developers

This is the first release built with full optimisation (`Release`);
everything up to `v1.0.1beta` shipped `MinSizeRel`, because `Release` did not
survive on hardware. Both build types now pass the same hardware gate.

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
