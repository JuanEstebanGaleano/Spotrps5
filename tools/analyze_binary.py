#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Spotrps5 contributors
"""Check a Linux x86-64 program against what the PS5 loader supports (a generic version of analyze_client.py).

  python3 tools/analyze_binary.py PROGRAM [--loader-src loader/src] [--markdown report.md]

Reports, for PROGRAM (an ELF):
  needed        shared libraries it asks for (the loader provides libc.so.6 and libz.so.1; anything else must be virtual)
  imports       undefined dynamic symbols; those without an adapter in the loader (BIND/BINDV/BINDZ tables) are listed as MISSING.
                MISSING is not fatal by itself: the loader binds such an import to a trap that only fails if the function is really called
                (the working PokeMMO client has 49 of them, e.g. fork and execve). What matters is which ones get called.
  relocations   relocation types the ELF loader must apply
  tls           a PT_TLS segment (needs a TLS block; %fs belongs to the PS5's libc)
  syscalls      raw `syscall` instructions, which bypass libc and would reach the PS5 kernel with Linux numbers
  fs accesses   %fs: references other than the stack canary
Needs binutils (readelf, objdump).
"""
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

SUPPORTED_RELOCATIONS = {"R_X86_64_64", "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT", "R_X86_64_RELATIVE"}
PROVIDED_LIBRARIES = {"libc.so.6", "libz.so.1"}
# Imports of the PokeMMO client that have no adapter and never get called (tools/client-baseline.txt minus the adapters).
KNOWN_UNCALLED = {"__cxa_finalize", "__environ", "__gmon_start__", "__libc_start_main", "environ", "execve", "execvp", "fork", "vfork", "posix_spawn",
                  "kill", "pthread_kill", "waitpid", "waitid", "stderr", "signal"}


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def supported_symbols(loader_src):
    names = set()
    for path in Path(loader_src).glob("*.c"):
        names.update(re.findall(r'BIND[VZ]?\("([A-Za-z0-9_]+)"', path.read_text(errors="replace")))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("program", type=Path)
    parser.add_argument("--loader-src", default=Path(__file__).resolve().parent.parent / "loader/src")
    parser.add_argument("--markdown", type=Path, help="also write the report as Markdown (for a CI job summary)")
    args = parser.parse_args()

    supported = supported_symbols(args.loader_src)
    out = []

    def say(line=""):
        out.append(line)

    dynamic = run("readelf", "-dW", str(args.program))
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", dynamic)
    say(f"# {args.program.name}")
    say()
    say(f"Loader adapters found: {len(supported)}")
    say()
    say("## Shared libraries (NEEDED)")
    for lib in needed:
        say(f"- {lib}: {'provided' if lib in PROVIDED_LIBRARIES else 'NOT PROVIDED: needs a virtual library'}")

    imports, weak = {}, set()
    for line in run("readelf", "--dyn-syms", "-W", str(args.program)).splitlines():
        parts = line.split()
        if len(parts) < 8 or parts[6] != "UND" or parts[3] not in ("FUNC", "OBJECT", "NOTYPE"):
            continue
        name = parts[7].split("@")[0]
        if not name:
            continue
        imports[name] = parts[3]
        if parts[4] == "WEAK":
            weak.add(name)
    missing = sorted(n for n in imports if n not in supported and n not in weak)
    missing_weak = sorted(n for n in imports if n not in supported and n in weak)
    say()
    say(f"## Imports: {len(imports)} undefined symbols, {len(missing)} MISSING (+{len(missing_weak)} weak)")
    for name in missing:
        say(f"- MISSING `{name}` ({imports[name]}){'  [also missing for PokeMMO: harmless unless called]' if name in KNOWN_UNCALLED else ''}")
    for name in missing_weak:
        say(f"- weak, missing `{name}`")

    relocations = collections.Counter(re.findall(r"\b(R_X86_64_[A-Z0-9_]+)", run("readelf", "-rW", str(args.program))))
    say()
    say("## Relocations")
    for kind, count in sorted(relocations.items()):
        say(f"- {kind}: {count}{'' if kind in SUPPORTED_RELOCATIONS else '  <- NOT HANDLED by the ELF loader'}")

    segments = run("readelf", "-lW", str(args.program))
    tls = [l for l in segments.splitlines() if l.strip().startswith("TLS")]
    say()
    say("## Thread-local storage")
    say(f"- PT_TLS: {'present: ' + ' '.join(tls[0].split()) if tls else 'none'}")

    disassembly = run("objdump", "-d", "--no-show-raw-insn", str(args.program)).splitlines()
    syscalls = sum(1 for l in disassembly if re.search(r"\bsyscall\b", l))
    fs_other = collections.Counter(m for l in disassembly for m in re.findall(r"%fs:(?!0x28\b)[^\s,)]*", l))
    canary = sum(1 for l in disassembly if "%fs:0x28" in l)
    say()
    say("## Raw system calls and %fs")
    say(f"- raw `syscall` instructions: {syscalls}")
    say(f"- %fs:0x28 (stack canary) reads: {canary}")
    say(f"- other %fs accesses: {sum(fs_other.values())} ({len(fs_other)} distinct)")
    for access, count in fs_other.most_common(10):
        say(f"  - `{access}` x{count}")

    text = "\n".join(out) + "\n"
    print(text)
    if args.markdown:
        args.markdown.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
