#!/usr/bin/env python3
"""Print a Markdown size report for the RP firmware.

Usage: firmware_size_report.py <rp.elf> <rp.elf.map> <title>

Used by the CI workflows to fill the job summary and printed after every local
link. Section sizes come from arm-none-eabi-size; the heap,
the stack, the code copied to RAM and the flash use come from the linker map,
so they follow memmap_rp.ld if it changes.
"""
import re
import subprocess
import sys

RAM_ORIGIN = 0x20000000
FLASH_ORIGIN = 0x10000000


def section_sizes(elf):
    out = subprocess.run(["arm-none-eabi-size", "-A", elf],
                         check=True, capture_output=True, text=True).stdout
    sizes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            sizes[parts[0]] = int(parts[1])
    return sizes


def map_symbol(map_text, name):
    match = re.search(r"^\s+(0x[0-9a-f]+)\s+(?:PROVIDE \()?" + re.escape(name) + r" = ",
                      map_text, re.M)
    if not match:
        sys.exit(f"symbol {name} not found in the map")
    return int(match.group(1), 16)


def code_in_ram(map_text, ram_end):
    """Sum the input sections of code that the linker placed in RAM."""
    total = 0
    pending = None
    one_line = re.compile(r"^ (\.(?:time_critical|text)[^\s]*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s")
    name_only = re.compile(r"^ (\.(?:time_critical|text)[^\s]*)\s*$")
    addr_line = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s")
    for line in map_text.splitlines():
        m = one_line.match(line)
        if m:
            addr, size = int(m.group(2), 16), int(m.group(3), 16)
            pending = None
        else:
            if name_only.match(line):
                pending = True
                continue
            m = addr_line.match(line) if pending else None
            pending = None
            if not m:
                continue
            addr, size = int(m.group(1), 16), int(m.group(2), 16)
        if RAM_ORIGIN <= addr < ram_end:
            total += size
    return total


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf, map_path, title = sys.argv[1:]
    sizes = section_sizes(elf)
    map_text = open(map_path, encoding="utf-8", errors="replace").read()
    stack_limit = map_symbol(map_text, "__StackLimit")
    rows = [
        (".text", sizes.get(".text", 0)),
        (".rodata", sizes.get(".rodata", 0)),
        (".data (copied to RAM)", sizes.get(".data", 0)),
        ("Code copied to RAM", code_in_ram(map_text, stack_limit)),
        (".bss", sizes.get(".bss", 0)),
        ("Heap (__end__ to __StackLimit)", stack_limit - map_symbol(map_text, "__end__")),
        ("Core-0 stack reserved",
         map_symbol(map_text, "__StackTop") - map_symbol(map_text, "__StackBottom")),
        ("Flash used", map_symbol(map_text, "__flash_binary_end") - FLASH_ORIGIN),
    ]
    print(f"### Firmware size: {title}\n")
    print("| Item | Bytes |")
    print("| --- | ---: |")
    for label, value in rows:
        print(f"| {label} | {value:,} |")


if __name__ == "__main__":
    main()
