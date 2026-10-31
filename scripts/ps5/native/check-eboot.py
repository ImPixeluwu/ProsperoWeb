#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the native title's linked PIE before it is converted to eboot.bin.

usage: check-eboot.py [--imports-out FILE] <llvm-bin-dir> <pie.elf> <stub.so | stub-dir>...

1. No syscall instruction is left in the image. A title may not execute it
   itself; the console ends the process ("directly issued a syscall", the
   Stremio PS5 port's native/kernel_mem.c). The native link drops libc.a's
   syscall objects (src/frontend/ps5/CMakeLists.txt) and
   src/frontend/ps5/native/native_runtime.c replaces what libkernel lacks.
2. Every import is exported by one of the link stubs (the converter needs
   that), and is reported by module. Imports from modules a title does not
   get at start-up (the module list a native media app's crash log showed on
   11.60, after the Stremio port's native/check_imports.py) are an error
   unless the UI loads that module itself before the first call.

3. Nothing the start-up code calls through can be 0 by construction: every
   init_array/fini_array entry is filled (by its content or a relocation),
   undefined weak symbols (resolved to 0 at link time) are only the known,
   harmless ones, and the GOT slots left at 0 match them.

--imports-out writes "<slot offset> <symbol>" per import slot (hex, no
prefix), which the title ships as imports.txt: its start-up code
(src/frontend/ps5/native/native_crt.cpp) names the import slots the system
left at 0 with it.

Exit status 0 when all checks pass.
"""
import glob
import os
import struct
import subprocess
import sys

# Undefined weak symbols the native link resolves to 0 on purpose.
KNOWN_WEAK_ZERO = {
    # libc++abi uses its own thread_local destructor list without it.
    "__cxa_thread_atexit_impl",
    # The TLS init hook of a thread_local without a dynamic initialiser.
    "_ZTHN3IPC10Connection20MessageDispatchScope9s_currentE",
}

# Loaded for a native app at start-up (Stremio PS5 native/check_imports.py).
STARTUP_MODULES = {
    "libkernel", "libkernel_web", "libkernel_sys", "libSceLibcInternal", "libc_stub_weak",
    "libSceSysmodule", "libSceAmpr", "libSceNet", "libSceIpmi", "libSceMbus", "libSceRegMgr",
    "libSceRtc", "libSceRazorCpu", "libSceAvSetting", "libSceVideoOut", "libSceAgcDriver",
    "libSceAgc", "libSceAudioOut", "libSceAudioIn", "libSceAjm", "libScePad", "libSceNetCtl",
    "libSceSsl", "libSceHttpCache", "libSceHttp", "libSceHttp2", "libSceNpCommon",
    "libSceNpManager", "libSceNpGameIntent", "libSceNpWebApi2", "libSceSaveData",
    "libSceSystemService", "libSceUserService", "libSceCommonDialog", "libSceSysUtil",
}
# Loaded by the UI with sceSysmoduleLoadModule before the first call
# (native/native_title.c, ps5_hwdec.cpp).
LOADED_BY_UI = {"libSceImeDialog", "libSceVideodec2"}


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def syscall_functions(objdump, pie):
    """Functions of the image that contain a syscall instruction."""
    proc = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", "--no-leading-addr", pie],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    found, function = {}, "?"
    for line in proc.stdout:
        if line and not line[0].isspace() and line.rstrip().endswith(">:"):
            function = line.strip()[1:-2]
        elif "\tsyscall" in line:
            found[function] = found.get(function, 0) + 1
    proc.wait()
    return found


def sections(readelf, pie):
    """name -> (address, file offset, size) of the allocated sections."""
    out = {}
    for line in run(readelf, "-S", "--wide", pie).splitlines():
        line = line.strip()
        if not line.startswith("[") or "]" not in line:
            continue
        fields = line.split("]", 1)[1].split()
        if len(fields) >= 5 and fields[0] != "Name":
            try:
                out[fields[0]] = (int(fields[2], 16), int(fields[3], 16), int(fields[4], 16))
            except ValueError:
                pass
    return out


def relocations(readelf, pie):
    """offset -> (type, symbol or addend)."""
    out = {}
    for line in run(readelf, "-r", "--wide", pie).splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[2].startswith("R_X86_64_"):
            try:
                offset = int(fields[0], 16)
            except ValueError:
                continue
            out[offset] = (fields[2], fields[4] if len(fields) > 4 else fields[3])
    return out


def startup_checks(nm, readelf, pie, imports_out):
    ok = True
    data = open(pie, "rb").read()
    secs = sections(readelf, pie)
    relocs = relocations(readelf, pie)
    if imports_out:
        with open(imports_out, "w", encoding="ascii") as f:
            for offset, (kind, symbol) in sorted(relocs.items()):
                if kind in ("R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT"):
                    f.write(f"{offset:x} {symbol}\n")
    for name in (".preinit_array", ".init_array", ".fini_array"):
        if name not in secs:
            continue
        address, offset, size = secs[name]
        empty = [address + i for i in range(0, size, 8)
                 if struct.unpack_from("<Q", data, offset + i)[0] == 0 and
                 not (address + i in relocs and relocs[address + i][0] == "R_X86_64_RELATIVE")]
        print(f"{name}: {size // 8} entries" + (f", EMPTY: {[hex(a) for a in empty]}" if empty else ""))
        ok &= not empty
    weak = sorted(line.split()[-1] for line in run(nm, pie).splitlines()
                  if line.split() and line.split()[0] in ("w", "v"))
    unknown = [name for name in weak if name not in KNOWN_WEAK_ZERO]
    print(f"undefined weak symbols (0 at run time): {len(weak)}" +
          (f"; UNEXPECTED: {' '.join(unknown)}" if unknown else ""))
    ok &= not unknown
    zero_got = []
    for name in (".got", ".got.plt"):
        if name not in secs:
            continue
        address, offset, size = secs[name]
        for i in range(0, size, 8):
            slot = address + i
            if slot in relocs or struct.unpack_from("<Q", data, offset + i)[0]:
                continue
            if name == ".got.plt" and i in (8, 16):
                continue  # the lazy-binding words, unused
            zero_got.append(slot)
    print(f"GOT slots 0 without a relocation: {len(zero_got)} "
          f"({' '.join(hex(a) for a in zero_got)})")
    if len(zero_got) > len(weak):
        print("ERROR: more zero GOT slots than undefined weak symbols")
        ok = False
    return ok


def main():
    args = sys.argv[1:]
    imports_out = None
    if len(args) >= 2 and args[0] == "--imports-out":
        imports_out, args = args[1], args[2:]
    if len(args) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    bindir, pie, stub_args = args[0], args[1], args[2:]
    nm = os.path.join(bindir, "llvm-nm")
    objdump = os.path.join(bindir, "llvm-objdump")
    ok = True

    stubs = []
    for arg in stub_args:
        stubs += sorted(glob.glob(os.path.join(arg, "*.so"))) if os.path.isdir(arg) else [arg]
    wanted = {line.split()[-1] for line in run(nm, "-D", "--undefined-only", pie).splitlines()
              if line.strip()}
    needed = {line.split("[")[1].split("]")[0].rsplit(".", 1)[0]
              for line in run(os.path.join(bindir, "llvm-readelf"), "-d", pie).splitlines()
              if "(NEEDED)" in line}
    owner = {}
    for so in stubs:
        module = os.path.basename(so)[:-3]
        if module not in needed:
            continue
        exports = {line.split()[-1] for line in run(nm, "-D", "--defined-only", so).splitlines()
                   if line.strip()}
        for name in exports & wanted:
            owner.setdefault(name, module)
    unresolved = sorted(wanted - owner.keys())
    by_module = {}
    for name, module in owner.items():
        by_module.setdefault(module, []).append(name)
    print(f"imports: {len(owner)} functions from {len(by_module)} modules")
    for module in sorted(by_module):
        state = ("start-up" if module in STARTUP_MODULES else
                 "loaded by the UI" if module in LOADED_BY_UI else "NOT LOADED")
        print(f"  {module}: {len(by_module[module])} ({state})")
        if state == "NOT LOADED":
            ok = False
            print("    " + " ".join(sorted(by_module[module])))
    if unresolved:
        ok = False
        print("ERROR: imports no stub exports: " + " ".join(unresolved))

    ok &= startup_checks(nm, os.path.join(bindir, "llvm-readelf"), pie, imports_out)

    found = syscall_functions(objdump, pie)
    if found:
        ok = False
        print("ERROR: syscall instructions in: " +
              ", ".join(f"{name} ({count})" for name, count in sorted(found.items())))
    else:
        print("syscall instructions: none")
    return 0 if ok else 1


sys.exit(main())
