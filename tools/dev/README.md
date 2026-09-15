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
