#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the native title's linked PIE before it is converted to eboot.bin.

usage: check-eboot.py <llvm-bin-dir> <pie.elf> <stub.so | stub-dir>...

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

Exit status 0 when both checks pass.
"""
import glob
import os
import subprocess
import sys

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


def main():
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    bindir, pie, stub_args = sys.argv[1], sys.argv[2], sys.argv[3:]
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

    found = syscall_functions(objdump, pie)
    if found:
        ok = False
        print("ERROR: syscall instructions in: " +
              ", ".join(f"{name} ({count})" for name, count in sorted(found.items())))
    else:
        print("syscall instructions: none")
    return 0 if ok else 1


sys.exit(main())
