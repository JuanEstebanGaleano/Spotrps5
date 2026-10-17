#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Spotrps5 contributors
"""Make a Linux x86-64 program use %gs instead of %fs for its thread-local storage.

  python3 tools/patch_tls.py PROGRAM OUTPUT

On Linux the thread pointer lives in %fs; on the PS5 %fs belongs to the system's own C library, so the loader cannot point it at a
TLS block for the program it runs. %gs is free there. This script finds every instruction that has a %fs segment override (the
disassembler gives the exact instruction boundaries, so no data is mistaken for code) and changes the override prefix byte from
0x64 (fs) to 0x65 (gs). Instruction lengths do not change. The loader then points %gs at a per-thread block laid out like glibc's
(variant II: the TLS data just below the thread pointer, a self pointer at gs:0, the stack-protector canary at gs:0x28).

The result is verified by disassembling it again: no %fs left, and exactly as many %gs overrides as were patched.
Needs binutils (readelf, objdump).
"""
import re
import subprocess
import sys
from pathlib import Path

PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def load_segments(path):
    segments = []
    for line in run("readelf", "-lW", str(path)).splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD":
            segments.append((int(parts[2], 16), int(parts[1], 16), int(parts[4], 16)))  # vaddr, offset, filesz
    return segments


def file_offset(segments, vaddr):
    for start, offset, size in segments:
        if start <= vaddr < start + size:
            return offset + vaddr - start
    raise ValueError(f"address {vaddr:#x} is not in a loaded segment")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    data = bytearray(source.read_bytes())
    segments = load_segments(source)
    patched = 0
    listing = run("objdump", "-d", "--no-show-raw-insn", "-w", str(source))
    raw_listing = run("objdump", "-d", "-w", str(source))
    for line in raw_listing.splitlines():
        if "%fs:" not in line:
            continue
        match = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)", line)
        if not match:
            continue
        vaddr = int(match.group(1), 16)
        raw = bytes.fromhex(match.group(2).replace(" ", ""))
        position = None
        for i, byte in enumerate(raw):
            if byte == 0x64:
                position = i
                break
            if byte not in PREFIXES:
                break
        if position is None:
            sys.exit(f"cannot find the fs prefix of the instruction at {vaddr:#x}: {line.strip()}")
        offset = file_offset(segments, vaddr) + position
        if data[offset] != 0x64:
            sys.exit(f"unexpected byte at {vaddr:#x}")
        data[offset] = 0x65
        patched += 1
    target.write_bytes(data)
    target.chmod(0o755)
    after = run("objdump", "-d", "--no-show-raw-insn", "-w", str(target))
    fs_left, gs_now = after.count("%fs:"), after.count("%gs:")
    expected_gs = listing.count("%gs:") + patched
    print(f"patched {patched} instructions: %fs left {fs_left}, %gs overrides {gs_now} (expected {expected_gs})")
    if fs_left or gs_now != expected_gs:
        sys.exit("verification failed")


if __name__ == "__main__":
    main()
