# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `programming.md` (full shared-region table and budget rules), `README.md` (user-facing setup menu, GEMDRIVE/Runner usage, HTTP API, project internals), `AGENTS.md` (overlapping playbook + troubleshooting table).

## What this repo is

Template for a **Sidecartridge Multi-device microfirmware app** targeting Atari ST / STE / MegaST(E). Each "app" is a UF2 image that runs on a Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge slot, emulating a ROM cartridge for the Atari while also handling networking, SD card I/O, and config. Public build/usage docs are at <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = release | debug   (case-insensitive; both are CMake Release — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

Required host environment:
- ARM GNU Toolchain 14.2 — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY (`pty=true`).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`; every script stops at the first failed step):
1. Copies `version.txt` into `rp/` and `target/atarist/`.
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces a **10 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 10240`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), empties `rp/build` and `rp/dist`, runs CMake, produces `rp/dist/rp-<board>.uf2` (`rp-<board>-debug.uf2` for debug). The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Iterating on m68k-only changes
For pure Atari-side edits (`target/atarist/src/*.s`, `*.ld`, `Makefile`, `inc/*.s`) where you only want vasm/vlink syntax validation and the cartridge size check, run `target/atarist/build.sh` directly instead of the top-level script:

```bash
cd target/atarist && ./build.sh "$(pwd)" release
```

The top-level `./build.sh` also re-pins SDK submodules and rebuilds the full RP firmware (CMake configure + lwIP/cyw43/mbedtls + UF2), which is minutes of work for a m68k syntax check. The atarist script alone enforces the 10 KB `BOOT.BIN` cap and prints `Cartridge code: N / 10240 bytes` — exactly what the iteration loop needs. Reserve the top-level build for RP-side changes or both-sides changes.

### Build gotchas
- **Both build types are CMake `Release` (`-O3`, `NDEBUG`).** `release` sets `DEBUG_MODE=0`; `debug` sets `DEBUG_MODE=1`, which only adds `DPRINTF` traces on the UART console (GPIO 0/1, **921,600 baud**; at the SDK's 115,200 the boot traces delayed the cartridge enough that a power-cycled ST booted into GEM). `DEBUG_BUFFERED_CONSOLE` in `rp/src/include/debug.h` (default 1) queues `DPRINTF` text in a 4 KB RAM ring sent by the UART transmit interrupt, so traces no longer stall the firmware; set it to 0 for the blocking console when chasing a hang, since text still queued when the firmware hangs is lost. Up to v1.0.1beta every build was `MinSizeRel`, because `Release` broke at runtime. **v1.1.0 ships `Release`**: both types pass the hardware gate — smoke, 4 MB transfers, aborted clients, and forced panic / HardFault / watchdog recovery. `RP_CMAKE_BUILD_TYPE=MinSizeRel` overrides only the CMake type, to compare against that configuration; it warns loudly, no workflow sets it, and it is comparison-only. A fixed `RELEASE_DATE` in the environment makes two builds of one commit byte-identical.
- `rp.elf` keeps its symbols (no `--strip-all`); the flashed image is the same either way. In VS Code, pick the `release` or `debug` variant from `.vscode/cmake-variants.yaml`, which sets `DEBUG_MODE` and the development UUID.
- `CHARACTER_GAP_MS` must remain defined (700) in `rp/src/include/blink.h` — removing it breaks the RP build.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context (CI, sub-shells, build wrappers). Without it the m68k build can fail silently and the previous `BOOT.BIN` survives — leading to a working RP firmware that displays garbage on the ST because `target_firmware.h` is stale.

### CI / release
- `.github/workflows/build.yml` builds `pico_w` `release` and `debug` on PR with ARM GNU Toolchain 14.2.rel1, writes a size report to the job summary (`.github/scripts/firmware_size_report.py`) and uploads the firmware with `rp.elf` and its map.
- `.github/workflows/release.yml` triggers on `v*` tags: builds `release`, attaches UF2 + JSON (plus `rp.elf` and its map) to the GitHub Release, uploads UF2 + JSON to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release).
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

### Tests
There is no test suite. "Verification" is: build succeeds, UF2 boots on hardware, manual interaction over the serial debug console.

Host-side developer tools live in `tools/dev/` (see its README). `tools/dev/console.py watch` captures the debug console to `tools/dev/logs/console.log`; read it with `console.py since-boot`, `grep` or `wait` instead of asking for a pasted log. `tools/dev/flash.sh <release|debug>` builds out of tree, flashes, and checks over SWD that the RP booted it and carries the new build ID (`<sha7>` or `<sha7>-dirty.<diff7>`, generated on every build by `rp/src/build_id.cmake`). The tools reach the RP only through picotool, the Debug Probe (`tools/dev/swd.py`) and the console, never through firmware services such as the HTTP server, so they stay portable to other microfirmwares. `tools/dev/swd.py` also reads the menu (`screen`, `text`), the shared variables and a crash report over SWD, and drives SELECT, keys and app commands; `tools/dev/smoke.py` is the end-to-end hardware check.

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge boot + dispatch + terminal. Lives at `$FA0000` in the ST address space (ROM4 cartridge region). Defines the cartridge header (`CA_MAGIC`, `CA_INIT`, …), command magic numbers, and the shared-variable layout used to talk to the RP2040.
- `src/devops.ld` — absolute layout of the cartridge image. The 10 KB budget (`CARTRIDGE_CODE_SIZE = $2800`) is split: `main.s` at `$0000` (2 KB), `gemdrive.s` at `$0800` (5 KB), `runner.s` at `$1C00` (3 KB). `main.s` mirrors the last two as `GEMDRIVE_BLOB` / `RUNNER_BLOB`.
- `src/gemdrive.s` — GEMDOS trap-#1 hooks + protocol blob. `gemdrive_install` copies `GEMDRIVE_BLOB_SIZE` bytes into RAM (default `screen_base - 16 KB`) once the user commits a mode, so the resident code survives cartridge teardown.
- `src/runner.s` — Runner foreground loop; self-relocates at `runner_entry` to just above the GEMDRIVE blob inside the same protected 16 KB region (`RUNNER_ABOVE_GEMDRIVE_OFFSET`).
- Dispatch: the RP writes `CMD_START = 4` ([G]) or `CMD_START_RUNNER = 5` ([U]) to the cartridge sentinel; the m68k's vsync-polled `check_commands` dispatches to `rom_function` / `runner_function`, both of which `jsr gemdrive_install` first.
- Adding more m68k modules: add a new `.text_<name>` section in `devops.ld`, mirror the offset with an `equ (ROM4_ADDR + $????)` in `main.s`, add the `.o` target to `target/atarist/Makefile`, and account for it in the 10 KB budget.
- Built via `stcmd make release` (m68k assembler in Docker); the cartridge image (header + all `.text_*` sections) must fit in 10 KB. A 64 KB padded copy is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 10 KB | m68k header + all `.text_*` sections (hard limit) |
| `$FA2800` | `CMD_MAGIC_SENTINEL` | 4 B | m68k polls here for NOP/RESET/command words |
| `$FA2804` | `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, 60 × 4 B indexed shared variables | ~768 B | fixed-offset metadata block (first 512 B until `$FA2B00`) |
| `$FA2B00` | `APP_FREE` | ~46 KB | contiguous arena for app buffers |
| `$FAE0C0` | `FRAMEBUFFER` | 8000 B | 320×200 monochrome framebuffer; sits at the top of the region so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block |

See `programming.md` for the full table and budget rules.

### RP2040 side (`rp/src/`)
- `main.c` — only sets clock/voltage, calls `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app via `reset_jump_to_booster()` to bootstrap. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — the application's main loop and entry point. This is where to add new features.
- `romemul.c` / `romemul.pio` — PIO programs and the runtime that emulates the cartridge ROM/RAM bus to the Atari (driven by `READ_*` / `WRITE_*` GPIOs defined in `include/constants.h`).
- `gconfig.c` / `aconfig.c` — global vs per-app configuration stored in dedicated flash sectors, on top of `settings/` (a key-value store).
- `network.c`, `httpc/`, `download.c` — Wi-Fi (CYW43, lwIP poll mode), HTTPS-capable HTTP client, firmware download support. The radio never sleeps: `PARAM_WIFI_POWER` is deliberately ignored and `CYW43_NONE_PM` is re-applied after every bring-up, because the driver puts its own default back on each STA re-enable. `network_superviseLink()` runs from the main loop and rejoins on its own after a link loss, backing off 5 s to 60 s; it also probes the default gateway by ARP every 60 s, because both `cyw43_tcpip_link_status()` and `cyw43_wifi_link_status()` can report a healthy link when the radio has left the network.
- `lwipopts.h` — the pool sizes carry the measurement that justifies them. `TCP_MSL` is 10 s, not lwIP's 60, because the server closes every connection and a 2-minute TIME_WAIT kept the pcb pool permanently full.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `display.c`, `display_term.c`, `term.c`, `u8g2/` — terminal-style display rendered into the Atari framebuffer at `$FAE0C0` and/or a local OLED.
- `blink.c`, `select.c`, `reset.c`, `tprotocol.c` — LED Morse status, SELECT-button handling, soft reset/jump-to-booster, transport protocol primitives.
- `health.c` — watchdog (8 s), crash and hang reboots with the reason kept in watchdog scratch registers 0-3, crash-loop guard, stack and heap high-water marks. Reported by `GET /api/v1/system/health` and shown on the menu's top line (`Recovered: hang in main_loop x2`). `sd_timeouts.c` overrides fatfs-sdk's weak SD timeout table so a failing card cannot outlast the watchdog.
- **`FF_FS_LOCK` is 28** (`rp/src/ff/ffconf.h`), not the default 8: it counts open files *and* open non-root directories, shared between GEMDRIVE (8 files + 16 searches) and the HTTP server (2 connections × a `FIL` and a `DIR`). `FR_TOO_MANY_OPEN_FILES` maps to GEMDOS `ENHNDL` and HTTP `503 too_many_open_files`.
- **The SD card is remounted automatically.** It is only mounted at boot, so a pulled card used to stay dead until a reset: with card-detect disabled nothing marks the drive uninitialised, and FatFs will not re-init a volume that is still registered. `sdcard_pollRemount()` reads sector 0 every 2 s to notice a pull (FatFs itself keeps serving from cache and reports no error), then `deinit()`s the card and re-mounts. While no card is mounted every SD endpoint answers `503 no_sd_card` and the menu refuses `[G]` and `[U]`.

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 192 K | Normal RAM (the upper 64 K was reclaimed in v1.1) |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The cartridge window the ST sees at `$FA0000` |
| `SCRATCH_X` / `SCRATCH_Y` | `0x20040000` / `0x20041000` | 4 K each | Core-local scratch |

Core 0's stack is 16 KB at the top of `RAM` (`__StackTop = 0x20030000`), guarded by the MPU
(`PICO_USE_STACK_GUARDS`), and the link fails if the heap floor and the stack would collide — three
`ASSERT`s in `memmap_rp.ld`. Measured stack peak across the v1.1 work is about 3.3 KB.

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). The PIO bus emulation runs hot — Core 0 also overclocks to 225 MHz at `VREG_VOLTAGE_1_10`.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override); the include path is set up so this file wins over the submodule's default.
- Don't touch `main.c` for feature work — start in `emul.c`.
- **The watchdog fires after 8 s without `health_feed()`.** It is fed only in the main loop, `emul_pollTick`, the Wi-Fi connect loop, the HTTP spin-waits and two slow boot steps. Any new loop or blocking call that can run longer must feed it and set a `health_setPhase()`; don't feed it anywhere else, or a real hang there goes unnoticed.
- `panic()` goes to `health_panic` (`PICO_PANIC_FUNCTION`), and the HardFault vector is replaced at boot. Both reboot the RP. Don't call `watchdog_reboot` or jump to Booster without going through `reset.c` / `reset.h`, which record the reason and stop the watchdog.
- **Never name an epic, story, iteration or task in anything that is committed** — comments, documentation, changelog, commit messages, PR descriptions. The planning notes live in `docs/epics/`, which is gitignored, so `EPIC-14 STORY-02` tells a reader of this repository nothing and cannot be looked up. Write what the code does and why instead: *"the driver re-applies its own default on every STA re-enable"*, not *"see EPIC-14 STORY-02"*. Check with `git grep -IiE "EPIC-|STORY-|\bepics?\b|\bstor(y|ies)\b"` before tagging a release.
- Match the existing C style (clang-format config in `.clang-format`, exposed as the `clang-format` CMake target when the binary is on `PATH`; clang-tidy config in `.clang-tidy`, used by editors, not by the build).
- **m68k modules `gemdrive.s` and `runner.s` MUST be 100% relocatable and self-contained.** They cannot rely on any cross-module symbol from `main.o` or each other. Concretely:
  - **No `xref` / `xdef`.** Every macro and helper they call must be defined inside the same assembly unit. The protocol macros live in `inc/sidecart_macros.s` and `bsr.w` into functions defined in `inc/sidecart_functions.s`; both files are `include`'d verbatim at the top/bottom of each module so the bsr's resolve to a private local copy. vasm doesn't export plain labels so vlink doesn't see duplicates between main.o, gemdrive.o, and runner.o.
  - **No `jsr` / `jmp` to outside-module symbols.** Both instructions emit absolute addresses, which freezes the call site to a specific runtime address — incompatible with relocation. Use `bsr` / `bra` (PC-relative) for all intra-module control flow. The single allowed exception is the entry-point `jmp` from `main.s`'s `check_commands` dispatch into the relocated blob (e.g. `jmp RUNNER_BLOB`); any future `jsr`/`jmp` to a non-local symbol from inside `gemdrive.s` or `runner.s` requires explicit user approval.

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
