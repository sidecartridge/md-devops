#!/usr/bin/env python3
"""
swd — read, check and drive a running RP2040 through the Debug Probe.

Single-file, stdlib-only (Python >= 3.10). Runs OpenOCD with the CMSIS-DAP
probe for each command. It never uses the firmware's own services: memory is
read through the debug port while the CPU keeps running, so it works for any
microfirmware from the template and on a hung RP. Only `key`, `app` and
`inject` need firmware help, the debug-only mailbox in devhooks.h.

Usage:
    python3 tools/dev/swd.py running ELF [--timeout S]
    python3 tools/dev/swd.py verify ELF
    python3 tools/dev/swd.py build-id [ELF ...]
    python3 tools/dev/swd.py read ADDRESS LENGTH OUTFILE
    python3 tools/dev/swd.py program ELF
    python3 tools/dev/swd.py resume
    python3 tools/dev/swd.py screen OUT.png [--elf ELF] [--scale N]
    python3 tools/dev/swd.py shared [--elf ELF] [--all]
    python3 tools/dev/swd.py select short|long|release [--hold-ms MS] [--force]
    python3 tools/dev/swd.py key CHAR [--shift] [--scan N] [--elf ELF]
    python3 tools/dev/swd.py app NAME [--elf ELF]
    python3 tools/dev/swd.py inject COMMAND_ID [WORD ...] [--elf ELF]

`running` waits until the vector table register (VTOR) holds the ELF's RAM
vector table, which the SDK's runtime init installs, and core 0 is not halted:
the RP has booted a firmware with that layout and got past its startup code.
`resume` releases both cores after a debugger left them halted. `verify`
compares the whole flashed image with the ELF (about 3 s for 630 KB).
`build-id` reads the `release_build_id` string from flash; with no ELF it tries
every ELF in tools/dev/builds/elf. `program` flashes the ELF and resets the RP.

`screen` renders the 320x200 framebuffer at the top of the 64 KB cartridge
window as a PNG: the setup menu as the ST shows it. `shared` prints the command
sentinel, the random token and the indexed shared variables, named after the
`*_SVAR_*` indexes in rp/src/include. Both find the window from the ELF's
`__rom_in_ram_start__` and the offsets from rp/src/include/chandler.h; without
`--elf` they use the cached ELF whose build ID matches the RP.

`select` presses the SELECT button: it forces the pin's input high through the
GPIO input override (IO_BANK0 GPIOn_CTRL.INOVER) for the hold time, so the
firmware sees a real press without any code of its own. `short` holds 300 ms,
`long` holds SELECT_LONG_RESET + 1 s (rp/src/include/select.h) and needs
`--force`, because in md-devops it erases the global settings. `release` clears
a stuck override.

`key`, `app` and `inject` need a debug build: they write the debug mailbox
(rp/src/include/devhooks.h) and wait until the main loop acknowledges it.
`key` sends a keystroke as the ST would, `inject` any protocol command with the
given 16-bit payload words after the random token, and `app` an app command
named by a DEVHOOKS_APP_<NAME> define, for example `app countdown_stop`.

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
import ast
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
INCLUDE_DIR = os.path.join(REPO, "rp", "src", "include")
FB_WIDTH, FB_HEIGHT = 320, 200
VTOR = 0xE000ED08
DHCSR = 0xE000EDF0
DHCSR_S_HALT = 1 << 17
DHCSR_RELEASE = 0xA05F0000  # DBGKEY; clears C_HALT and C_DEBUGEN
CORES = ("rp2040.core0", "rp2040.core1")
IO_BANK0 = 0x40014000
INOVER_SHIFT = 16
INOVER_HIGH = 3
SELECT_SHORT_MS = 300
# DevhooksMailbox (rp/src/include/devhooks.h): offsets of its fields.
MAILBOX_SYMBOL = "devhooksMailbox"
MAILBOX_MAGIC = 0x444B4831
MB_SEQ, MB_ACK, MB_KIND, MB_RESULT, MB_CMD, MB_SIZE, MB_PAYLOAD = (
    4, 8, 12, 16, 20, 22, 24)
MAILBOX_WORDS = 16
KIND_PROTOCOL, KIND_APP = 1, 2
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


def read_word(address: int, core: str = CORES[0]) -> int:
    out = openocd(f"targets {core}", f"mdw 0x{address:08x}")
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
            halted = read_word(DHCSR) & DHCSR_S_HALT
            if vtor in tables and not halted:
                print(f"running: VTOR 0x{vtor:08x}")
                return 0
            last = f"VTOR 0x{vtor:08x}" + (", core 0 halted" if halted else "")
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


def header_defines(path: str) -> dict[str, int]:
    """Integer #defines of a C header, resolving references between them."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text).replace("\\\n", " ")
    raw = dict(re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)[ \t]+([^\n]+)$",
                          text, re.M))
    values: dict[str, int] = {}

    def resolve(name: str, depth: int = 0) -> int | None:
        if name in values:
            return values[name]
        if name not in raw or depth > 20:
            return None
        expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", raw[name])
        for ref in set(re.findall(r"\b[A-Za-z_]\w*\b", expr)):
            v = resolve(ref, depth + 1)
            if v is None:
                return None
            expr = re.sub(rf"\b{ref}\b", str(v), expr)
        try:
            tree = ast.parse(expr.strip(), mode="eval")
        except SyntaxError:
            return None
        allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant,
                   ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div,
                   ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd, ast.USub)
        if not all(isinstance(n, allowed) for n in ast.walk(tree)):
            return None
        v = eval(compile(tree, path, "eval"), {"__builtins__": {}})
        if not isinstance(v, (int, float)):
            return None
        values[name] = int(v)
        return values[name]

    for name in raw:
        resolve(name)
    return values


def matching_elf(explicit: str | None) -> str:
    """The ELF given, or the cached ELF whose build ID the RP carries."""
    if explicit:
        return explicit
    elfs = sorted(glob.glob(os.path.join(HERE, "builds", "elf", "*.elf")),
                  key=os.path.getmtime, reverse=True)
    for elf in elfs:
        expected = elf_build_id(elf)
        if expected and read_build_id(elf) == expected:
            return elf
    raise SwdError("no cached ELF matches the RP's build ID: pass --elf")


def cartridge_window(elf: str) -> tuple[int, dict[str, int]]:
    base = elf_symbols(elf, "__rom_in_ram_start__").get("__rom_in_ram_start__")
    if not base:
        raise SwdError(f"{elf} has no __rom_in_ram_start__")
    return base[0], header_defines(os.path.join(INCLUDE_DIR, "chandler.h"))


def write_png(path: str, width: int, height: int, rows: list[bytes]) -> None:
    """8-bit greyscale PNG."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + row for row in rows)
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def cmd_screen(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    base, defs = cartridge_window(elf)
    offset = defs["CHANDLER_FRAMEBUFFER_OFFSET"]
    size = defs["CHANDLER_FRAMEBUFFER_SIZE"]
    if size != FB_WIDTH * FB_HEIGHT // 8:
        raise SwdError(f"framebuffer is {size} bytes, expected 320x200 mono")
    fb = read_memory(base + offset, size)
    stride = FB_WIDTH // 8
    rows = []
    for y in range(FB_HEIGHT):
        line = fb[y * stride:(y + 1) * stride]
        # Bit 7 of each byte is the leftmost of its eight pixels. Lit pixels
        # are drawn black on white, as on the ST's monochrome screen.
        pixels = bytes(0 if (line[x >> 3] >> (7 - (x & 7))) & 1 else 255
                       for x in range(FB_WIDTH))
        scaled = bytes(p for p in pixels for _ in range(args.scale))
        rows.extend([scaled] * args.scale)
    write_png(args.out, FB_WIDTH * args.scale, FB_HEIGHT * args.scale, rows)
    lit = sum(bin(b).count("1") for b in fb)
    print(f"wrote {args.out} ({FB_WIDTH * args.scale}x{FB_HEIGHT * args.scale}, "
          f"{lit} pixels lit) from 0x{base + offset:08x}")
    return 0


def svar_names() -> dict[int, list[str]]:
    names: dict[int, list[str]] = {}
    for header in sorted(glob.glob(os.path.join(INCLUDE_DIR, "*.h"))):
        defs = header_defines(header)
        for name, value in defs.items():
            if "_SVAR_" in name or name in ("CHANDLER_HARDWARE_TYPE",
                                            "CHANDLER_SVERSION",
                                            "CHANDLER_BUFFER_TYPE"):
                names.setdefault(value, []).append(name)
    return names


def cmd_shared(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    base, defs = cartridge_window(elf)
    start = defs["CHANDLER_SHARED_BLOCK_OFFSET"]
    var_off = defs["CHANDLER_SHARED_VARIABLES_OFFSET"]
    slots = defs["CHANDLER_SHARED_VARIABLES_SLOTS"]
    data = read_memory(base + start, var_off - start + slots * 4)

    def long_at(offset: int) -> int:
        # The ST reads big-endian longs made of two RP-order 16-bit words.
        hi, lo = struct.unpack_from("<HH", data, offset - start)
        return (hi << 16) | lo

    print(f"cartridge window 0x{base:08x} (ST $FA0000), ELF {os.path.basename(elf)}")
    for label, name in (("command sentinel", "CHANDLER_CMD_SENTINEL_OFFSET"),
                        ("random token", "CHANDLER_RANDOM_TOKEN_OFFSET"),
                        ("random token seed", "CHANDLER_RANDOM_TOKEN_SEED_OFFSET")):
        off = defs[name]
        print(f"  ${0xFA0000 + off:06X}  {label:<34} 0x{long_at(off):08x}")
    names = svar_names()
    for i in range(slots):
        value = long_at(var_off + i * 4)
        label = " / ".join(names.get(i, []))
        if value == 0 and not label and not args.all:
            continue
        print(f"  ${0xFA0000 + var_off + i * 4:06X}  [{i:2}] {label or '-':<29} "
              f"0x{value:08x}  {value}")
    return 0


def cmd_read(args: argparse.Namespace) -> int:
    data = read_memory(int(args.address, 0), int(args.length, 0))
    with open(args.outfile, "wb") as f:
        f.write(data)
    print(f"read {len(data)} bytes from {args.address} into {args.outfile}")
    return 0


def cmd_resume(args: argparse.Namespace) -> int:
    """Release both cores from a debug halt. OpenOCD's own resume fails in a
    new OpenOCD run, and a halted core 1 also pauses the RP2040's timer and
    watchdog, which leaves core 0 asleep forever."""
    commands = []
    for core in CORES:
        commands += [f"targets {core}", f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}"]
    openocd(*commands)
    states = [read_word(DHCSR, core) & DHCSR_S_HALT for core in CORES]
    if any(states):
        print("still halted: " + ", ".join(
            c for c, h in zip(CORES, states) if h), file=sys.stderr)
        return 3
    print("both cores released")
    return 0


def include_defines() -> dict[str, int]:
    defs: dict[str, int] = {}
    for header in sorted(glob.glob(os.path.join(INCLUDE_DIR, "*.h"))):
        defs.update(header_defines(header))
    return defs


def cmd_select(args: argparse.Namespace) -> int:
    defs = include_defines()
    gpio = defs["SELECT_GPIO"]
    ctrl = IO_BANK0 + 4 + 8 * gpio
    normal = read_word(ctrl) & ~(3 << INOVER_SHIFT)
    if args.press == "release":
        openocd(f"mww 0x{ctrl:08x} 0x{normal:08x}")
        print(f"SELECT (GPIO {gpio}) override cleared")
        return 0
    if args.press == "long" and not args.force:
        raise SwdError("a long press can erase settings (md-devops erases "
                       "the global config, Wi-Fi included): add --force")
    hold = args.hold_ms or (SELECT_SHORT_MS if args.press == "short"
                            else defs["SELECT_LONG_RESET"] + 1000)
    pressed = normal | (INOVER_HIGH << INOVER_SHIFT)
    try:
        openocd(f"mww 0x{ctrl:08x} 0x{pressed:08x}", f"sleep {hold}",
                f"mww 0x{ctrl:08x} 0x{normal:08x}")
    finally:
        # Never leave the button pressed, even if OpenOCD failed mid-hold.
        if read_word(ctrl) & (3 << INOVER_SHIFT):
            openocd(f"mww 0x{ctrl:08x} 0x{normal:08x}")
    print(f"SELECT (GPIO {gpio}) held {hold} ms")
    return 0


def mailbox_request(elf: str, kind: int, command_id: int, words: list[int],
                    timeout: float = 5.0) -> int:
    """Send one request through the debug mailbox; return its result."""
    if len(words) > MAILBOX_WORDS:
        raise SwdError(f"at most {MAILBOX_WORDS} payload words")
    sym = elf_symbols(elf, MAILBOX_SYMBOL).get(MAILBOX_SYMBOL)
    if not sym:
        raise SwdError(f"{os.path.basename(elf)} has no {MAILBOX_SYMBOL}: "
                       "a debug build is needed")
    base = sym[0]
    magic, seq, ack = struct.unpack("<III", read_memory(base, 12))
    if magic != MAILBOX_MAGIC:
        raise SwdError(f"no mailbox at 0x{base:08x} (magic 0x{magic:08x})")
    if seq != ack:
        raise SwdError("the previous request was never acknowledged: "
                       "is the main loop running?")
    commands = [f"mww 0x{base + MB_KIND:08x} {kind}",
                f"mwh 0x{base + MB_CMD:08x} {command_id}",
                f"mwh 0x{base + MB_SIZE:08x} {len(words) * 2}"]
    for i, word in enumerate(words):
        commands.append(f"mwh 0x{base + MB_PAYLOAD + 2 * i:08x} {word & 0xFFFF}")
    # seq last: the firmware acts as soon as seq differs from ack.
    commands.append(f"mww 0x{base + MB_SEQ:08x} {ack + 1}")
    openocd(*commands)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new_ack, _, result = struct.unpack(
            "<III", read_memory(base + MB_ACK, MB_RESULT + 4 - MB_ACK))
        if new_ack == ack + 1:
            return result
        time.sleep(0.1)
    raise SwdError(f"no acknowledge within {timeout:g} s: "
                   "is the main loop running?")


def send_protocol(elf: str, command_id: int, words: list[int]) -> None:
    # The payload starts with the random token, which only matters to the ST.
    for _ in range(10):
        if mailbox_request(elf, KIND_PROTOCOL, command_id, [0, 0] + words):
            return
        time.sleep(0.1)
    raise SwdError("the firmware kept another command pending")


def cmd_key(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    defs = include_defines()
    if len(args.char) != 1:
        raise SwdError("CHAR must be one character")
    command_id = defs["APP_TERMINAL"] | defs["APP_TERMINAL_KEYSTROKE"]
    param = (ord(args.char) | (args.scan << defs["TERM_KEYBOARD_SCAN_SHIFT"]) |
             ((1 if args.shift else 0) << defs["TERM_KEYBOARD_SHIFT_SHIFT"]))
    send_protocol(elf, command_id, [param & 0xFFFF, param >> 16])
    print(f"key {args.char!r} sent")
    return 0


def cmd_inject(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    send_protocol(elf, int(args.command_id, 0),
                  [int(w, 0) for w in args.words])
    print(f"command {args.command_id} injected")
    return 0


def cmd_app(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    name = "DEVHOOKS_APP_" + args.name.upper()
    command_id = include_defines().get(name)
    if command_id is None:
        raise SwdError(f"no {name} define in rp/src/include")
    result = mailbox_request(elf, KIND_APP, command_id, [])
    print(f"{name}: result {result}")
    return 0 if result else 3


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

    rs = sub.add_parser("resume", help="release both cores from a debug halt")
    rs.set_defaults(func=cmd_resume)

    se = sub.add_parser("select", help="press the SELECT button")
    se.add_argument("press", choices=("short", "long", "release"))
    se.add_argument("--hold-ms", type=int)
    se.add_argument("--force", action="store_true",
                    help="allow a long press")
    se.set_defaults(func=cmd_select)

    k = sub.add_parser("key", help="send a keystroke as the ST would")
    k.add_argument("char")
    k.add_argument("--shift", action="store_true")
    k.add_argument("--scan", type=int, default=0)
    k.add_argument("--elf")
    k.set_defaults(func=cmd_key)

    ap = sub.add_parser("app", help="send an app command (DEVHOOKS_APP_*)")
    ap.add_argument("name")
    ap.add_argument("--elf")
    ap.set_defaults(func=cmd_app)

    ij = sub.add_parser("inject", help="inject a protocol command")
    ij.add_argument("command_id")
    ij.add_argument("words", nargs="*")
    ij.add_argument("--elf")
    ij.set_defaults(func=cmd_inject)

    sc = sub.add_parser("screen", help="render the framebuffer as a PNG")
    sc.add_argument("out")
    sc.add_argument("--elf")
    sc.add_argument("--scale", type=int, default=2)
    sc.set_defaults(func=cmd_screen)

    sh = sub.add_parser("shared", help="print the shared variables")
    sh.add_argument("--elf")
    sh.add_argument("--all", action="store_true",
                    help="also print unnamed slots that are zero")
    sh.set_defaults(func=cmd_shared)
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
