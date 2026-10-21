# Changelog

## Unreleased

Run programs from the GEM desktop, feed data to a program while it runs,
and load programs that GEMDRIVE used to crash on.

### Run programs from the GEM desktop: the GEMDRIVE Runner

`[G]` now brings the Runner along. The Atari boots on through its AUTO
folder to the desktop as usual, and a desk accessory, `DEVOPS.ACC`, takes
`runner run`, `load`, `exec`, `unload` and `cd` from there. Programs started
this way find the AUTO-folder drivers, the cookie jar and GEM already
loaded; `[U]` starts too early for any of them.

- **On by default.** `Ru[n]ner` in the GEMDRIVE section of the menu shows
  it, and `[N]` turns it off, which makes `[G]` plain GEMDRIVE as before.
- **Nothing to install.** The firmware writes `DEVOPS.ACC` to the GEMDRIVE
  folder itself, and deletes it again when the Runner is off. With
  GEMDRIVE as `C:`, GEM loads it from there; with another drive letter,
  copy it to the root of your boot drive.
- **`runner status`** says whether the accessory has started. After a
  `runner reset`, wait for it before the next command.
- **On the Atari**, the accessory's Desk menu entry shows the current
  directory and the last command with its result.
- **Limits.** It starts plain TOS programs, not GEM ones, and `runner res`
  is refused under GEM. GEM gives the accessory no turn while a dialog or
  menu is open or something is being dragged, so commands wait until then.

Tested on a Mega STE with TOS 2.06, with and without an ACSI2STM hard disk,
and in Hatari with EmuTOS.

### The AUTO folder runs from GEMDRIVE

With GEMDRIVE as `C:` and no other hard disk, nothing in `C:\AUTO` ran,
because TOS looked for it on drive A:. GEMDRIVE now makes `C:` the current
drive as it installs, so AUTO-folder drivers load. With a hard disk whose
driver takes the boot drive, such as an ACSI2STM, TOS runs that drive's
AUTO folder instead.

### Programs that GEMDRIVE could not load

- **Programs with an odd file size** crashed in the loader (three bombs) or
  started corrupted, because the last byte of every odd-length read arrived
  wrong. They now load, and an odd-length `Fread` gets its last byte right.
- **Programs with an odd-sized symbol table** bombed in the loader with an
  address error. They now load, as they do with TOS's own loader.
- **A read over 4 KB** that did not end on a 4 KB boundary could write up to
  4 KB past the end of the program's buffer. It now stops where it should.
- **Files and searches left open by a crashed program** no longer pile up.
  After a few crashes GEMDRIVE ran out of handles, and GEM could not even
  load its accessories, until the cartridge was restarted. Every Atari boot
  now starts clean.
- **The current directory** reached the Atari with its bytes swapped:
  `Dgetpath` returned `\STDL` as `S\DT`.

### Files that programs open through GEMDRIVE

- **`Fseek` never worked**: the RP took the handle for the offset, so a seek
  answered "invalid handle" or moved the wrong file. Programs built with
  mintlib seek on every file they open, and `fopen(..., "a")` failed and left
  the file locked. A seek past either end of a file is now refused with
  `ERANGE`, as by TOS, instead of growing the file.
- **`Fdatime`** went by the current drive rather than the file handle, and
  its "get" returned whatever was in memory. It now acts on the handle's
  file: get returns the file's own date and time, and set changes them.
- **A file a program left open** when it ended, or crashed, stayed open
  until the next boot, locked against every other open, including the
  API's. GEMDRIVE now closes a program's files when it ends, as TOS does.
- **On a Mega STE with its cache on**, as programs that want speed run,
  GEMDRIVE calls were lost or answered wrong: the cache kept old copies of
  what the cartridge said. GEMDRIVE now turns the cache off for each call
  it answers and back on after; the CPU speed is left alone. The cartridge
  also turns it off when the Atari boots, which a reset does not.

### Feed data to a running program: the rom4 area

1 KB of the cartridge, at `$FA8B00`, that the workstation writes over HTTP
(`PUT /api/v1/rom4`, or `sidecart rom4 write`) and any program on the
Atari can read while it runs, in any mode. Programs find it through shared
variables 20–22. Several writes can go in one request and are applied in
the order given, so a program can be handed a complete update.

### The boot countdown remembers your choice

When nobody pressed a key, the countdown always started `[U]`. It now
starts whichever of `[G]` and `[U]` you used last, so a power cycle comes
back the way it went, and the countdown bar says which: `Booting GEMDRIVE
in 20 s`. Until you first use `[G]` it is still `[U]`.

### Hold SELECT at power-on to go to the Booster

As in other SidecarTridge apps: hold SELECT while switching on and the
cartridge goes straight to the Booster, to install another app or update
this one.

### The setup menu

- **GEMDRIVE settings changed in the menu** — the drive letter, the
  relocation address, `_memtop` and the Runner's hook vector — only took
  effect from the next boot, so choosing `D:` and pressing `[G]` still
  gave GEMDRIVE `C:`. `[G]` and `[U]` now use what the menu shows.

### The Runner

- **On a Mega STE with its cache on**, the Runner's commands could be
  missed, or their answers lost. `runner reset`, `meminfo` and the other
  commands a running program cannot refuse are now seen within about half
  a second whatever the program does with the cache, and DEVOPS.ACC turns
  the cache off while it talks to the cartridge, for a desktop set up to
  run fast. A program the accessory starts still gets the desktop's cache.
- **`runner meminfo`** gave the address of TOS's ROM header as the running
  program's basepage. It now reports the real one.
- **A `load` or `unload` that timed out** was still carried out later, when
  the Atari next looked, after the API had reported it as failed. It is now
  withdrawn.

### The API

- **Every 503 reply** carries `Retry-After: 1`, as documented. Three kinds
  of error left it out.
- **The Runner's *busy* reply** had an empty body, so the CLI could only
  print `error (503):`. It now carries the usual error message.

### For developers

- `QUESTION.TOS` is a known-answer test for the Runner: it writes
  `ANSWER.TXT` containing `42` and exits with 42. `ROM4CHK.TOS` prints the
  rom4 area to the debug stream.
- The cartridge code is at 10,190 of its 10,240 bytes.

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
