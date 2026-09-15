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
tools/dev/flash.sh debug                  # build, flash with picotool, check over SWD
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
ID is stored in flash as the `release_build_id` string, and `rp.elf` is kept as
`tools/dev/builds/elf/<type>-<id>.elf` for resolving crash addresses later.

Flashing uses `picotool load -f -x`, which reboots the running firmware into BOOTSEL over USB; when
picotool cannot see the RP it falls back to the Debug Probe. Then `flash.sh` checks the result over
SWD with `swd.py`: the RP booted the ELF, its flash matches the ELF byte for byte, and it carries
the new build ID. On failure it exits with 1 and prints the console since the last boot.

## Debug probe: `swd.py`

The tools talk to the RP only through picotool, the Debug Probe and the console UART, never through
the firmware's own services (its HTTP server, for example), so they work with any microfirmware
built from this template and with a hung RP. Memory is read while the CPU keeps running.

```bash
python3 tools/dev/swd.py running tools/dev/builds/debug/rp.elf   # booted this firmware?
python3 tools/dev/swd.py verify tools/dev/builds/debug/rp.elf    # flash identical to the ELF?
python3 tools/dev/swd.py build-id                                # which build is on the RP?
python3 tools/dev/swd.py read 0x2003e0c0 8000 fb.bin             # dump memory
python3 tools/dev/swd.py program tools/dev/builds/debug/rp.elf   # flash through the probe
```

`build-id` with no ELF tries the ELFs in `tools/dev/builds/elf`. OpenOCD is `$OPENOCD`, `openocd`
on `PATH`, or `../pico/openocd/src/openocd`; its scripts come from `$PICO_OPENOCD_PATH`, the
variable `.vscode/launch.json` uses. A command that fails on a momentary debug-port drop (common
while the firmware changes its clock early in boot) is retried. Close a VS Code debug session
first: only one program can use the probe.
