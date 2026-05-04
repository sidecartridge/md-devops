# md-devops — Atari ST developer microfirmware

A SidecarTridge Multi-device microfirmware app that turns the
cartridge slot of an Atari ST / STE / Mega ST(E) into a remote
development surface for the m68k. While it boots, the cartridge
emulates a ROM, mounts a directory off the microSD card as a
GEMDOS drive (GEMDRIVE), and exposes a Wi-Fi HTTP API + CLI that
lets a workstation **upload / download files, launch programs,
load and step through them in pieces, switch screen rez, snapshot
ST memory, and stream debug bytes from the running m68k in real
time**.

If you want the basic GEMDRIVE-only experience (folder-as-drive,
floppies, ACSI), the sister app
[md-drives-emulator](https://github.com/sidecartridge/md-drives-emulator)
is what you want. **md-devops** is the developer-focused variant
of that surface — same drive emulation, plus the runner / debug /
HTTP-management story stitched on top.

## Highlights

- **GEMDRIVE folder-as-drive** — point a microSD subdirectory at
  a TOS drive letter; it just appears.
- **Runner mode** — workstation triggers `Pexec` of any TOS / PRG,
  watches the exit code, can also `cd`, switch resolution, snapshot
  memory.
- **`Pexec` load / exec / unload split** — load once, re-exec many
  times, free explicitly. Useful for iterative debugging without
  re-reading a slow file off SD each cycle.
- **Advanced Runner** — a second command surface running from
  inside the m68k's VBL ISR, so it keeps working when the
  foreground program has wedged the system (infinite loops,
  bombs already painted, traps disabled).
- **Remote HTTP file management** — `curl` + `sidecart` CLI for
  ls / get / put / rm / mv / mkdir / rmdir / mvdir / volume.
- **Fast debug traces** — single-cartridge-cycle byte emit from
  m68k (`*(volatile char *)(0xFBFF00 + c)`), captured RP-side and
  streamed to either an HTTP `tail -f` endpoint or USB CDC. No
  framing, no overhead, byte-exact.
- **Live setup menu** — graphical status icons (Wi-Fi / SD / USB
  CDC / Adv Vector), animated countdown bar, USB CDC attach state
  refreshed live as you plug / unplug.

# ⚠️ Read before installing

This README documents end-user installation + usage. The
*template* repo this app was created from is documented at
<https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>;
that's the doc you want if you're building your own
microfirmware app rather than running this one.

The HTTP API has **no authentication**. Treat the network the
device is reachable on as trusted; don't expose `sidecart.local`
past your LAN router.

## 🚀 Installation

md-devops is delivered as a UF2 image installed on top of the
SidecarTridge Multi-device's **Booster** app. You don't flash it
to the Pico W directly — Booster does that for you on demand.

1. Boot the SidecarTridge Multi-device into Booster (the default
   boot target on a fresh device).
2. From a workstation on the same LAN, open Booster's web
   interface in a browser at
   `http://sidecart.local/` — the IP shown on the ST's screen
   also works.
3. Open the **Apps** tab and pick **DevOps**. (If you don't see
   it, check Booster's "Catalog URL" config; the public catalog
   is at `https://atarist.sidecartridge.com/apps.json`.)
4. **Download** copies the UF2 + JSON descriptor onto the
   microSD card.
5. **Launch** activates it — the Pico W reboots into md-devops.
   Subsequent power-ons boot directly into md-devops; you only
   need to revisit Booster to install another app or update.

## 🕹️ Usage — boot flow

Power-on after install lands on the **Setup menu** for ~20
seconds. From there you have four top-level commands:

| Key | Action |
| --- | --- |
| `[U]` | **Runner mode** (recommended). GEMDRIVE comes up, plus the Runner control surface for `runner run` / `load` / `exec` / etc. |
| `[E]` or `[F]` | Same as `[U]` for GEMDRIVE-only purposes — ST drops straight into the emulated drive — but does **not** activate the Runner. Use this if you only want file emulation and don't need the workstation to drive the ST. |
| `[X]` | Return to the Booster menu (e.g. to install another app). |
| any key | Halt the auto-launch countdown so the menu stays up indefinitely while you read it. |

If you don't press anything within ~20 s, the firmware **auto-fires
[U]Runner** — Runner is the more useful default for unattended
boots.

## ⚙️ Setup menu screen

The menu paints into the cartridge framebuffer at `$FAE0C0` so
the ST itself shows it. Top to bottom:

```
DevOps Microfirmware - vX.Y.Z              ← title bar (inverted)

GEMDRIVE                                   💾
  F[o]lder    : /devops
  [D]rive     : C:
  [R]eloc addr: auto (screen-8KB)
  Mem[t]op    : auto (matches reloc)

Adv [V]ector                               ⚙
  Hook        : etv_timer ($400)

API Endpoint                               📶
  URL         : http://sidecart.local/
  IP address  : 192.168.1.42

USB CDC (Debug serial)                     💡
  Status      : connected

[E]xit (launch)  r[U]nner  [X] Booster
Select an option: ▌
[████████░░░░░░░░░░] Booting in 12 s — any key halts
```

| Section | What it shows | Keys |
| --- | --- | --- |
| **GEMDRIVE** | Which microSD folder is mounted as the emulated drive, the drive letter assigned to it, the GEMDRIVE relocation address (`auto` = `screen_base − 8 KB`), and the patched `_memtop` value. The hard-drive icon appears whenever the section is live. | `[o]` change folder, `[d]` change drive letter, `[r]` change reloc addr, `[t]` change `_memtop`. |
| **Adv [V]ector** | Which interrupt vector the Advanced Runner installs its hook into — `vbl ($70)` (faster, fires on every VBL) or `etv_timer ($400)` (more compatible). The cog icon appears whenever the section is live. | `[V]` toggle between `vbl` / `etv_timer`. |
| **API Endpoint** | mDNS hostname and the IP DHCP leased. The Wi-Fi icon appears once the network is up; if there's no IP yet (Wi-Fi still associating) the icon is hidden. | (read-only) |
| **USB CDC (Debug serial)** | `connected` / `disconnected` — live-refreshed as you plug or unplug a USB cable into the Pico. The lightbulb icon flips in lock-step. | (read-only) |
| **Bottom navigation strip** | Top-level command keys + a one-character prompt area for typing them. | `[E]` / `[U]` / `[X]` (and `[F]` does the same as `[E]`). |
| **Animated countdown bar** | Shrinking white bar; the message "Booting in N s — any key halts" is overlaid in inverted colour so it stays readable both halves. Becomes "Countdown stopped. Press [E], [U] or [X] to continue." once any key has been pressed. | (passive — but pressing any key halts the countdown) |

## 🌐 Remote HTTP Management API + the `sidecart.py` CLI

Once the device joins Wi-Fi it serves an HTTP/1.1 REST API on
port 80 at `http://sidecart.local/` (mDNS hostname is the
gconfig `PARAM_HOSTNAME`, default `sidecart`). The IP is also
shown on the setup-menu's API Endpoint line if mDNS isn't
resolving on your workstation. The API is the **single
control surface** for every remote operation in this firmware
— file management, program execution, debug streaming. Everything
the rest of this README documents (`ping`, `gemdrive`, `runner`,
`debug`) is a thin wrapper around HTTP calls to this service.

| Family | HTTP endpoints | CLI prefix |
| --- | --- | --- |
| Health | `GET /api/v1/ping` | `sidecart ping` |
| GEMDRIVE | `GET/PUT/DELETE/POST /api/v1/gemdrive/{volume,files,folders}/…` | `sidecart gemdrive …` |
| Runner | `GET/POST /api/v1/runner/…` | `sidecart runner …` |
| Debug | `GET /api/v1/debug`, `GET /api/v1/debug/log` | `sidecart debug …` |

You can drive the API directly from any HTTP client (`curl`,
`wget`, `httpie`, a browser, a shell script, your editor's
REST plugin) — `cli/sidecart.py` is a convenience layer, not
the only supported access path. The full per-endpoint reference
lives in [`docs/api.md`](docs/api.md): every URL, JSON envelope
shape, status code, error vocabulary, and `curl` example.

> ⚠️ **The HTTP API has no authentication.** Treat the network
> the device is reachable on as trusted. Don't expose
> `sidecart.local` past your LAN router.

### `cli/sidecart.py` — the Python CLI

A single-file Python script in this repo. It's the easiest way
to use the API from a workstation: every verb maps 1-to-1 to an
HTTP call, error envelopes are unwrapped to human-readable
text, exit codes carry status information so shell scripts can
branch.

**Requirements**: Python 3.10 or later. **No third-party
packages needed** — the script is stdlib-only (`urllib`,
`http.client`, `json`, `argparse`, `os`, `sys`). If `python3`
finds a recent-enough interpreter, you're done.

**"Installation"**: there is none. The script lives at
`cli/sidecart.py` in this repo — clone the repo (or just
download that one file) and call it directly:

```sh
git clone https://github.com/sidecartridge/md-devops.git
cd md-devops
python3 cli/sidecart.py ping
```

If you prefer not to type `python3 cli/sidecart.py` every time,
make it executable and stick it on your `PATH`:

```sh
chmod +x cli/sidecart.py
ln -s "$(pwd)/cli/sidecart.py" /usr/local/bin/sidecart
sidecart ping
```

(The script's shebang is `#!/usr/bin/env python3`, so a symlink
or copy works the same as the `python3 …` form.)

**Configuring the host**. The CLI talks to `sidecart.local` by
default. Override that anywhere your mDNS doesn't reach:

```sh
# Per-invocation flag.
python3 cli/sidecart.py --host 192.168.1.42 ping
python3 cli/sidecart.py --host my-pico:8080 ping

# Or environment variable for a whole shell session.
export SIDECART_HOST=192.168.1.42
python3 cli/sidecart.py ping
```

Precedence: `--host` > `$SIDECART_HOST` > `sidecart.local`.

**Global flags** (apply to every subcommand):

| Flag | Effect |
| --- | --- |
| `--host HOST[:PORT]` | Override the device address (default `sidecart.local`). |
| `--json` | Print the raw JSON envelope on stdout instead of human-friendly text. Useful for scripting + piping into `jq`. |
| `-q` / `--quiet` | Silence successful-output text; errors still go to stderr. |
| `-h` / `--help` | Per-subcommand help (e.g. `sidecart gemdrive put --help`). |

**Exit codes** (so shell scripts can branch on category):

| Code | Meaning |
| --- | --- |
| `0` | Success. |
| `1` | Generic / unexpected error. |
| `2` | argparse usage error (bad flag, missing required arg). |
| `3` | Server returned `404`. |
| `4` | Server returned `409`. |
| `5` | Server returned `400` / `422` / other 4xx. |
| `6` | Server returned `503` (busy or SD not mounted). |
| `7` | Server returned 5xx other than `503`. |
| `8` | Network / DNS error (couldn't reach the host). |

**Driving the API directly without the CLI**. Two examples to
make the equivalence concrete:

```sh
# CLI form:
python3 cli/sidecart.py ping

# Direct HTTP equivalent:
curl http://sidecart.local/api/v1/ping
```

```sh
# CLI form (upload a file, overwriting if it exists):
python3 cli/sidecart.py gemdrive put GAME.TOS -f

# Direct HTTP equivalent:
curl --upload-file ./GAME.TOS \
     'http://sidecart.local/api/v1/gemdrive/files/GAME.TOS?overwrite=1'
```

Pick whichever fits your workflow. The next chapters show the
CLI form because it's terser, but every example has a `curl`
counterpart documented in [`docs/api.md`](docs/api.md).

## 🛜 First-contact: `ping`

Once the device has joined Wi-Fi (the API Endpoint section of
the menu shows a leased IP and the wifi icon is lit), the very
first thing to test is `ping` — it confirms the workstation can
reach the device at all and reports the firmware version + boot
uptime. No state is touched by `ping`; it's safe to run any
time.

```sh
$ python3 cli/sidecart.py ping
ok  version=v0.0.1dev  uptime=42s
```

```sh
$ python3 cli/sidecart.py --json ping
{"ok":true,"version":"v0.0.1dev","uptime_s":42}
```

```sh
$ curl http://sidecart.local/api/v1/ping
{ "ok": true, "version": "v0.0.1dev", "uptime_s": 42 }
```

If `ping` fails (`sidecart`: `EXIT_NETWORK = 8`; `curl`:
connection refused / DNS failure), the rest of the API is
unreachable too — fix Wi-Fi / mDNS first. Common causes:

- **mDNS not resolving** — try the literal IP shown on the
  setup-menu's API Endpoint line (`http://192.168.1.42/`).
  macOS resolves `.local` natively; Linux usually needs
  `avahi-daemon` running; Windows needs Bonjour Print Services
  or `nss-mdns`.
- **Wrong host** — override with the `--host` flag or the
  `SIDECART_HOST` env var:
  ```sh
  python3 cli/sidecart.py --host 192.168.1.42 ping
  SIDECART_HOST=192.168.1.42 python3 cli/sidecart.py ping
  ```
- **Different network segment** — the workstation has to be on
  the same LAN broadcast domain (mDNS doesn't cross routers).

Once `ping` works, every other CLI command works too — they all
talk to the same HTTP server.

## 💾 GEMDRIVE commands — manage files and folders remotely

The Atari ST sees a microSD subdirectory as a TOS drive (default
`C:`, configurable from the setup menu). The `gemdrive`
subcommand on the workstation gives you full read/write access
to that same directory tree without ejecting the SD card —
useful for iterating on a `BUILD.PRG`, copying down assets the
ST has produced, or just keeping the contents in sync with a
project folder.

All paths are jailed under the `GEMDRIVE_FOLDER` aconfig
parameter (default `/devops`) — the API can never read or write
outside that root. FAT 8.3 names are enforced (stem ≤ 8 chars,
extension ≤ 3 chars, ASCII, no `*?/\:<>"|+,;=[]`); see
[`docs/api.md`](docs/api.md#conventions) for the full
constraints.

### `gemdrive volume` — disk capacity

```sh
$ python3 cli/sidecart.py gemdrive volume
free  : 1.0 GB
total : 8.0 GB
fs    : FAT32
```

### `gemdrive ls [PATH]` — list a folder

`PATH` defaults to `/`. Columns: name, size, is_dir, mtime.

```sh
$ python3 cli/sidecart.py gemdrive ls /
INVADERS.PRG  12345  -  2026-04-30T12:34:56
DATA          0      d  2026-04-29T08:00:00

$ python3 cli/sidecart.py gemdrive ls /DATA
SAVE.DAT      4096   -  2026-05-01T11:22:33

$ python3 cli/sidecart.py --json gemdrive ls /
{"ok":true,"path":"/","entries":[{"name":"INVADERS.PRG", …}], …}
```

### `gemdrive get REMOTE [LOCAL]` — download a file

`LOCAL` defaults to the basename of `REMOTE`. `-r/--resume`
sends a `Range:` header to continue a partial download.

```sh
$ python3 cli/sidecart.py gemdrive get GAME.TOS                  # → ./GAME.TOS
$ python3 cli/sidecart.py gemdrive get GAME.TOS local.tos        # custom local name
$ python3 cli/sidecart.py gemdrive get GAME.TOS -r               # resume after Ctrl-C
```

Progress prints to stderr (`123 KB / 4096 KB`); silence with
`-q`.

### `gemdrive put LOCAL [REMOTE]` — upload a file

`REMOTE` defaults to the basename of `LOCAL`. `-f/--force`
overwrites if the remote file exists; without it, an existing
target returns `409 conflict`.

```sh
$ python3 cli/sidecart.py gemdrive put GAME.TOS                  # → /GAME.TOS (fail if exists)
$ python3 cli/sidecart.py gemdrive put GAME.TOS -f               # overwrite
$ python3 cli/sidecart.py gemdrive put build/GAME.TOS /GAMES/ -f # different remote dir
```

Per-request cap: 4 MB. `Content-Length` is required;
`Transfer-Encoding: chunked` uploads are rejected.

### `gemdrive rm REMOTE` — delete a file

```sh
$ python3 cli/sidecart.py gemdrive rm STALE.TXT
ok
```

Deleting a directory through `rm` returns `404 is_directory`
with a hint to use `rmdir`. Files that the ST currently has
open (e.g. a TOS program reading from a save file) return
`409 conflict`.

### `gemdrive mv FROM TO` — rename / move a file

```sh
$ python3 cli/sidecart.py gemdrive mv OLD.TXT NEW.TXT
$ python3 cli/sidecart.py gemdrive mv /TMP/A.PRG /GAMES/A.PRG    # cross-folder
```

Both endpoints must stay in the same drive (the API can't move
across drives). The destination must not exist (no implicit
overwrite — `gemdrive rm` first if you need to replace).

### `gemdrive mkdir REMOTE` — create a folder

```sh
$ python3 cli/sidecart.py gemdrive mkdir /GAMES
$ python3 cli/sidecart.py gemdrive mkdir /GAMES/ARKANOID
```

Parent must exist. Folder name follows the same FAT 8.3 rules
as files (stem ≤ 8 chars, no extension typically, ASCII).

### `gemdrive rmdir REMOTE` — delete an empty folder

```sh
$ python3 cli/sidecart.py gemdrive rmdir /GAMES/EMPTY
```

Refuses non-empty folders with `409 conflict` — `gemdrive ls`
to inspect, `gemdrive rm` each file first.

### `gemdrive mvdir FROM TO` — rename / move a folder

```sh
$ python3 cli/sidecart.py gemdrive mvdir OLDNAME NEWNAME
$ python3 cli/sidecart.py gemdrive mvdir /TMP/STAGE /GAMES/STAGE  # move sub-tree
```

Refuses moving a folder into one of its own descendants
(`422 unprocessable`).

### Putting it together — a typical edit-build-test cycle

```sh
# Build on the workstation, push, run, watch the result.
make game.tos
python3 cli/sidecart.py gemdrive put game.tos /GAMES/ -f
python3 cli/sidecart.py runner run /GAMES/game.tos
python3 cli/sidecart.py runner status        # check exit code
```

Full HTTP / `curl` reference + every error code lives in
[`docs/api.md`](docs/api.md). The HTTP API has **no
authentication** — treat the network it's reachable on as
trusted; don't expose it past your LAN.

## Runner mode

Runner mode is the foreground execution path the firmware ships
with. The user picks `[U]` at the setup menu to launch it; the
m68k Runner stays in a poll loop waiting for commands from the
RP, while GEMDRIVE keeps servicing TOS file I/O so launched
programs can use the emulated drive normally.

The Runner exposes its own subset of the HTTP API
(`/api/v1/runner/*`) and CLI (`sidecart runner …`) for
cold-resetting the ST, executing programs, navigating the cwd,
switching screen resolutions, and reading a live system-memory
snapshot. See [`docs/api.md`](docs/api.md) for the full reference
(curl + sidecart examples for every endpoint).

```sh
python3 cli/sidecart.py runner status
python3 cli/sidecart.py runner cd /GAMES/ARKANOID
python3 cli/sidecart.py runner run RUNME.TOS
python3 cli/sidecart.py runner reset
python3 cli/sidecart.py runner res low
python3 cli/sidecart.py runner meminfo
```

### Pexec lifecycle (load / exec / unload)

For interactive-debugger-style workflows, the firmware also
splits GEMDOS Pexec mode 0 into three separate verbs so a
program can be loaded once and re-executed many times before
its memory is released:

```sh
python3 cli/sidecart.py runner load /HELLODBG.TOS    # Pexec(3) — load only, returns basepage
python3 cli/sidecart.py runner exec                   # Pexec(4) — run the loaded program
python3 cli/sidecart.py runner exec                   # re-exec on the same basepage
python3 cli/sidecart.py runner unload                 # Mfree the basepage when done
```

Strict-refuse: a second `load` while a program is still loaded
returns `409 program_already_loaded` — `unload` it (or restart
Runner mode) before submitting another. See
[`docs/api.md`](docs/api.md#pexec-lifecycle-load--exec--unload)
for the full lifecycle, mode-by-mode mapping, and error
envelope.

### Advanced Runner

A second command surface runs from inside the m68k's VBL ISR
(`$70`, or `$400` if you switched `ADV_HOOK_VECTOR` to
`etv_timer`), so it keeps working when a launched program has
wedged the foreground poll loop — infinite loops, bombs already
painted, traps disabled. Two of the three POSTs (`adv jump`,
`adv load`) require the VBL hook specifically; `adv meminfo`
works on either vector. None gate on the busy lock.

```sh
python3 cli/sidecart.py runner adv status                              # is the hook installed? which vector?
python3 cli/sidecart.py runner adv meminfo                             # snapshot from inside the ISR
python3 cli/sidecart.py runner adv jump 0xFA1C00                       # rte to a user address
python3 cli/sidecart.py runner adv load ./kernel.bin 0x40000           # stream a file into RAM
```

`runner adv jump` and `runner adv load` accept addresses in
decimal, `0xhex`, or `$hex` — quote `$hex` in your shell
(`'$FA1C00'`) or the shell will eat it as a variable reference.
Prefer `0xhex` in scripts.

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
> 1. The setup-menu countdown starts running with no ST
>    attached, and once it elapses (~20 s by default) the
>    firmware auto-commits Runner mode. When you subsequently
>    power the ST, it sees the cartridge already in Runner
>    state and skips the menu — you never get a chance to
>    press `[U]` / `[E]` / `[F]`.
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
python3 cli/sidecart.py gemdrive put target/atarist/test/hello-debug/dist/HELLODBG.TOS /
python3 cli/sidecart.py runner run /HELLODBG.TOS
# → "Hello, world!\n" × 1000 appears on whichever transport you're watching.
```

The capture is gated on firmware-mode commit (`[U]` / `[E]` /
`[F]` in the menu) so menu activity never pollutes the stream.
Full endpoint reference + multi-consumer model is in
[`docs/api.md`](docs/api.md#debug-traces).

## Project internals

This section documents the on-cartridge memory map and module
layout for contributors and ST programmers writing apps that
talk to md-devops directly. End users running the firmware
don't need any of this.

### Shared 64 KB region layout

The cartridge presents a 64 KB window at m68k `$FA0000`–`$FAFFFF`,
mirrored at RP `0x20030000`. Layout:

- The cartridge image (m68k header + code) lives in the first
  **10 KB** (`$FA0000`–`$FA27FF`). `target/atarist/build.sh`
  enforces this with a hard size check on `BOOT.BIN`.
- A small fixed-offset metadata block (`CMD_MAGIC_SENTINEL`,
  `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, 60 × 4-byte indexed
  shared variables) sits at `$FA2800`.
- The **`APP_FREE`** arena (~46 KB at `$FA2B00`) is the
  contiguous space the app uses for its own buffers.
- The **framebuffer** (8000 B for 320×200 monochrome) sits at
  the very top of the region (`$FAE0C0`), so an overrun walks
  off the end of the 64 KB window instead of corrupting the
  metadata block.

Both sides derive every offset symbolically from the constants
in `rp/src/include/chandler.h` (RP-side) and
`target/atarist/src/main.s` (m68k side). Apps must never
hard-code an address inside the region — always reference the
named offset/symbol so the layout stays the single source of
truth.

See `programming.md` for the full table and budget rules.

### Cartridge code layout

The cartridge image is split via `target/atarist/src/devops.ld`
into three sections:

- `main.s` at offset `0x0000` (`$FA0000`, 2 KB) — boot,
  dispatch, terminal.
- `gemdrive.s` at offset `0x0800` (`$FA0800`, 5 KB) — GEMDOS
  hooks + protocol blob (relocated to RAM at boot).
- `runner.s` at offset `0x1C00` (`$FA1C00`, 3 KB) — Runner
  foreground loop.

`main.s`'s `check_commands` dispatch reads the cartridge
sentinel and hands control to the right blob: `CMD_START = 4`
jumps into `GEMDRIVE_BLOB+4` (diagnostic + memtop verify), and
`CMD_START_RUNNER = 5` jumps into `RUNNER_BLOB` (the Runner's
poll loop). Adding a new module follows the same pattern: place
a new `.text_<name>` section in `devops.ld`, mirror the offset
with an `equ` in `main.s`, and add the `.o` target to
`target/atarist/Makefile`.

Both `gemdrive.s` and `runner.s` are 100 % relocatable and
self-contained — they include `inc/sidecart_macros.s` and
`inc/sidecart_functions.s` privately so the protocol `bsr`'s
resolve locally inside their own object files. No `xref` /
`xdef` cross-module references; no `jsr` / `jmp` to
outside-module symbols (except the entry-point `jmp` from
`main.s`'s dispatch). See `CLAUDE.md` for the full editing
guardrails.

### Building from source

The full build (m68k cartridge image + RP firmware UF2) is
driven by `./build.sh <board_type> <build_type> <APP_UUID>`
from the repo root. See
<https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>
for toolchain prerequisites (ARM GNU 14.2 toolchain,
`atarist-toolkit-docker` for the m68k side) and
`programming.md` / `CLAUDE.md` for the iteration playbook.

For m68k-only changes, run `target/atarist/build.sh` directly —
it enforces the 10 KB `BOOT.BIN` cap and is much faster than
the top-level build (which also re-pins SDK submodules and
recompiles the entire RP firmware).

## License

The source code of the project is licensed under the GNU
General Public License v3.0. The full license is accessible in
the [LICENSE](LICENSE) file.
