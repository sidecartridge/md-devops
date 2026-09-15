#!/usr/bin/env python3
"""
swd — read and check a running RP2040 through the Debug Probe.

Single-file, stdlib-only (Python >= 3.10). Runs OpenOCD with the CMSIS-DAP
probe for each command. Nothing here needs the firmware's cooperation: memory
is read through the debug port while the CPU keeps running, so it works for any
microfirmware and on a hung RP.

Usage:
    python3 tools/dev/swd.py running ELF [--timeout S]
    python3 tools/dev/swd.py verify ELF
    python3 tools/dev/swd.py build-id [ELF ...]
    python3 tools/dev/swd.py read ADDRESS LENGTH OUTFILE
    python3 tools/dev/swd.py program ELF

`running` waits until the vector table register (VTOR) holds the ELF's RAM
vector table, which the SDK's runtime init installs: the RP has booted a
firmware with that layout and got past its startup code. `verify` compares the whole flashed image with the ELF (about 3 s
for 630 KB). `build-id` reads the `release_build_id` string from flash; with no
ELF it tries every ELF in tools/dev/builds/elf. `program` flashes the ELF and
resets the RP.

OpenOCD is $OPENOCD, `openocd` on PATH, or ../pico/openocd/src/openocd next to
the repo; its scripts come from $PICO_OPENOCD_PATH (as in .vscode/launch.json),
else the tcl/ folder of a source build.

Exit codes:
    0  success
    1  generic / unexpected, or OpenOCD failed
    2  argparse usage error
    3  check failed: not running the ELF, image differs, or ID not found
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
VTOR = 0xE000ED08
BUILD_ID_SYMBOL = "release_build_id"


class SwdError(Exception):
    pass


def openocd_command() -> list[str]:
    ocd = os.environ.get("OPENOCD") or shutil.which("openocd")
    if not ocd:
        local = os.path.join(REPO, "..", "pico", "openocd", "src", "openocd")
        if os.access(local, os.X_OK):
            ocd = local
    if not ocd:
        raise SwdError("no openocd: set OPENOCD")
    cmd = [ocd]
    scripts = os.environ.get("PICO_OPENOCD_PATH") or os.path.join(
        os.path.dirname(os.path.realpath(ocd)), "..", "tcl")
    if os.path.isfile(os.path.join(scripts, "interface", "cmsis-dap.cfg")):
        cmd += ["-s", scripts]
    return cmd + ["-f", "interface/cmsis-dap.cfg", "-f", "target/rp2040.cfg",
                  "-c", "adapter speed 5000"]


# The debug port can drop for a moment, for example while the firmware changes
# the clock and core voltage early in boot. OpenOCD then reports one of these.
TRANSIENT = re.compile(r"Failed to read memory|Error connecting DP|"
                       r"Examination failed|DP initialisation failed")
ATTEMPTS = 4


def openocd(*commands: str, check: bool = True) -> str:
    """Run OpenOCD with `init`, the commands and `exit`; return its output.
    A run that failed on a transient debug-port error is repeated."""
    args = openocd_command() + ["-c", "init"]
    for c in commands:
        args += ["-c", c]
    args += ["-c", "exit"]
    for attempt in range(ATTEMPTS):
        proc = subprocess.run(args, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        failed = proc.returncode != 0 or "Failed to read memory" in out
        if not (failed and TRANSIENT.search(out)) or attempt == ATTEMPTS - 1:
            break
        time.sleep(0.5)
    if check and proc.returncode != 0:
        lines = [l for l in out.splitlines()
                 if l.startswith("Error") and "algo" not in l]
        raise SwdError("openocd failed: " + (" / ".join(lines[-3:]) or
                                             out.strip()[-300:]))
    return out


def read_word(address: int) -> int:
    out = openocd(f"mdw 0x{address:08x}")
    m = re.search(rf"0x{address:08x}:\s+([0-9a-fA-F]{{8}})", out)
    if not m:
        raise SwdError(f"cannot read 0x{address:08x}")
    return int(m.group(1), 16)


def read_memory(address: int, length: int) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "dump.bin")
        openocd(f"dump_image {path} 0x{address:08x} {length}")
        with open(path, "rb") as f:
            data = f.read()
    if len(data) != length:
        raise SwdError(f"read {len(data)} of {length} bytes")
    return data


def elf_symbols(elf: str, *names: str) -> dict[str, tuple[int, int]]:
    """Address and size of each named symbol present in the ELF."""
    nm = shutil.which("arm-none-eabi-nm")
    if not nm:
        raise SwdError("arm-none-eabi-nm not on PATH")
    out = subprocess.run([nm, "-S", elf], capture_output=True, text=True,
                         check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in names:
            found[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        elif len(parts) == 3 and parts[2] in names:
            found.setdefault(parts[2], (int(parts[0], 16), 0))
    return found


def cmd_running(args: argparse.Namespace) -> int:
    # boot2 points VTOR at the flash vector table before the firmware starts,
    # at the same address in every build; the SDK's runtime init then moves it
    # to RAM. Only the RAM table proves this firmware got past its startup.
    syms = elf_symbols(args.elf, "ram_vector_table", "__vectors")
    table = syms.get("ram_vector_table") or syms.get("__vectors")
    if not table:
        raise SwdError(f"{args.elf} has no vector table symbol")
    tables = {table[0]}
    deadline = time.monotonic() + args.timeout
    last = "no answer"
    while True:
        try:
            vtor = read_word(VTOR)
            if vtor in tables:
                print(f"running: VTOR 0x{vtor:08x}")
                return 0
            last = f"VTOR 0x{vtor:08x}"
        except SwdError as exc:
            last = str(exc)
        if time.monotonic() >= deadline:
            print(f"not running {args.elf} after {args.timeout:g} s ({last})",
                  file=sys.stderr)
            return 3
        time.sleep(0.5)


def cmd_verify(args: argparse.Namespace) -> int:
    out = openocd(f"verify_image {args.elf}", check=False)
    m = re.search(r"verified (\d+) bytes in ([\d.]+)s", out)
    if m:
        print(f"flash matches {args.elf} ({m.group(1)} bytes)")
        return 0
    diffs = len(re.findall(r"^diff \d+ address", out, re.M))
    if diffs:
        print(f"flash differs from {args.elf} (at least {diffs} bytes)",
              file=sys.stderr)
        return 3
    raise SwdError("verify failed: " + out.strip()[-300:])


def read_build_id(elf: str) -> str | None:
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym or sym[1] == 0:
        return None
    data = read_memory(sym[0], sym[1])
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def cmd_build_id(args: argparse.Namespace) -> int:
    elfs = args.elf or sorted(
        glob.glob(os.path.join(HERE, "builds", "elf", "*.elf")),
        key=os.path.getmtime, reverse=True)
    tried = set()
    for elf in elfs:
        sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
        if not sym or sym in tried:
            continue
        tried.add(sym)
        build_id = read_build_id(elf)
        expected = elf_build_id(elf)
        if build_id and (args.elf or build_id == expected):
            print(build_id)
            return 0
    print("no build ID found at the release_build_id address of "
          f"{len(tried)} ELF layout(s)", file=sys.stderr)
    return 3


def elf_build_id(elf: str) -> str | None:
    """The build ID string stored in the ELF file itself."""
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym:
        return None
    headers = subprocess.run(["arm-none-eabi-objdump", "-h", elf],
                             capture_output=True, text=True, check=True).stdout
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
                         headers, re.M):
        name, size, vma = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if vma <= sym[0] < vma + size:
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "section.bin")
                subprocess.run(["arm-none-eabi-objcopy", "-O", "binary",
                                f"--only-section={name}", elf, path],
                               check=True)
                with open(path, "rb") as f:
                    f.seek(sym[0] - vma)
                    data = f.read(sym[1])
            return data.split(b"\0", 1)[0].decode("ascii", errors="replace")
    return None


def cmd_read(args: argparse.Namespace) -> int:
    data = read_memory(int(args.address, 0), int(args.length, 0))
    with open(args.outfile, "wb") as f:
        f.write(data)
    print(f"read {len(data)} bytes from {args.address} into {args.outfile}")
    return 0


def cmd_program(args: argparse.Namespace) -> int:
    openocd(f"program {args.elf} verify reset")
    print(f"flashed {args.elf}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="swd.py", description="Read and check a running RP2040 over SWD.")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("running", help="wait until the RP runs the ELF")
    r.add_argument("elf")
    r.add_argument("--timeout", type=float, default=30.0)
    r.set_defaults(func=cmd_running)

    v = sub.add_parser("verify", help="compare the flash with the ELF")
    v.add_argument("elf")
    v.set_defaults(func=cmd_verify)

    b = sub.add_parser("build-id", help="read the build ID from flash")
    b.add_argument("elf", nargs="*")
    b.set_defaults(func=cmd_build_id)

    m = sub.add_parser("read", help="dump a memory range to a file")
    m.add_argument("address")
    m.add_argument("length")
    m.add_argument("outfile")
    m.set_defaults(func=cmd_read)

    g = sub.add_parser("program", help="flash the ELF and reset")
    g.add_argument("elf")
    g.set_defaults(func=cmd_program)
    return p


def main() -> int:
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (SwdError, subprocess.CalledProcessError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
