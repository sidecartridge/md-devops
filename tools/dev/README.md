# Developer tools

Host-side tools for working on md-devops with the hardware attached: a SidecarTridge Multi-device
on an Atari ST, with a Raspberry Pi Debug Probe wired to the RP2040's SWD pins and to its debug UART
(GPIO 0/1). Python tools use the standard library only.

## Debug console: `console.py`

Captures the debug console of a `debug` build (921,600 baud) to `tools/dev/logs/console.log`, with
a timestamp on every line, and shows it in the terminal. Use it instead of a serial terminal such
as CoolTerm: only one program can open the port.

```bash
python3 tools/dev/console.py watch          # leave running in a terminal
```

`watch` finds the Debug Probe by its USB name (`--port` to choose another device), waits for it when
it is unplugged, and reopens it when it returns. While it runs, other commands read the log:

```bash
python3 tools/dev/console.py since-boot                     # everything since the last boot
python3 tools/dev/console.py since-boot --boot 2            # the boot before that
python3 tools/dev/console.py tail 100
python3 tools/dev/console.py grep 'PANIC|HardFault' --since-boot
python3 tools/dev/console.py wait 'GEMDRIVE folder .* ready' --timeout 30
```

`grep` and `wait` take Python regular expressions and exit with 3 when nothing matches. `wait` only
matches lines that arrive after it starts, so start it before the action that should print the
line. The log rotates to `console.log.1` at 32 MB.

The global settings print corrupt key bytes at boot, so macOS `grep` may treat the log as binary
and print nothing; use `console.py grep` or `grep -a`.

## Build, flash and verify: `flash.sh`

```bash
tools/dev/flash.sh debug                  # build, flash with picotool, wait for the new build
tools/dev/flash.sh release --probe        # flash through the Debug Probe instead
tools/dev/flash.sh debug --build-only     # build only
tools/dev/flash.sh debug --src /tmp/src   # build a copy of rp/src (for example a patched linker script)
```

Builds out of tree in `tools/dev/builds/<type>`, incrementally: about 35 s the first time, a few
seconds after that. It does not touch `rp/build` or the submodules, and warns when a submodule is
not at the version `rp/build.sh` pins. The m68k image is not rebuilt: after changing
`target/atarist`, run `target/atarist/build.sh` first.

Every build carries a build ID: the git commit, `<sha7>`, or `<sha7>-dirty.<diff7>` when the tree
has uncommitted changes. The same tree always gives the same ID and a byte-identical binary. The
ID is printed on the console at boot (`health: build …`) and returned by
`GET /api/v1/system/health` with the build type, and `rp.elf` is kept as
`tools/dev/builds/elf/<type>-<id>.elf` for resolving crash addresses later.

After flashing, `flash.sh` polls the health report for up to 90 s (`--timeout`) until it shows the
new ID and type. If it does not, it exits with 1 and prints the console since the last boot.

Flashing uses `picotool load -f -x`, which reboots the running firmware into BOOTSEL over USB. When
picotool cannot see the RP it falls back to OpenOCD through the Debug Probe. OpenOCD is `$OPENOCD`,
`openocd` on `PATH`, or `../pico/openocd/src/openocd`; its scripts come from `$PICO_OPENOCD_PATH`,
the variable `.vscode/launch.json` uses.
