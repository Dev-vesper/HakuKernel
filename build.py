#!/usr/bin/env python3
"""MyOS v0.1 build — one straightforward script, no shell-quoting games.

Usage:
    python build.py            build myos.img only
    python build.py run        build + boot in a QEMU window
    python build.py headless   build + boot headless, verify via serial log

Pipeline:
    kernel.c --cl--> kernel.obj  +  entry.asm --nasm--> entry.obj
    entry.obj + kernel.obj --link--> kernel.pe --pe2bin--> kernel.bin
    boot.asm --nasm (-DKERNEL_ENTRY from the PE)--> boot.bin (512 B)
    boot.bin + kernel.bin --mkimg--> myos.img (1.44 MB floppy)

All tools are invoked with argument lists (never through cmd.exe), so paths
with spaces just work.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
os.makedirs(BUILD, exist_ok=True)


def log(msg):
    print(msg, flush=True)


def find_tool(name, fallbacks, label):
    found = shutil.which(name)
    if found:
        return found
    for path in fallbacks:
        if os.path.isfile(path):
            return path
    sys.exit(f"Tool not found: {label}")


def find_msvc():
    pattern = (r"C:\Program Files\Microsoft Visual Studio\2022"
               r"\*\VC\Tools\MSVC\*\bin\Hostx64\x86\cl.exe")
    cands = sorted(glob.glob(pattern))
    if not cands:
        sys.exit('x86 cl.exe not found: install VS2022 "Desktop development with C++"')
    return cands[-1]


def run(cmd):
    log("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    nasm = find_tool("nasm", [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), r"bin\NASM\nasm.exe"),
        r"C:\Program Files\NASM\nasm.exe",
    ], "NASM  [winget install NASM.NASM]")
    cl = find_msvc()
    link = os.path.join(os.path.dirname(cl), "link.exe")
    if not os.path.isfile(link):
        sys.exit(f"link.exe not found next to cl.exe: {cl}")
    py = sys.executable
    qemu = find_tool("qemu-system-i386",
                     [r"C:\Program Files\qemu\qemu-system-i386.exe"], "QEMU")
    log(f"NASM: {nasm}")
    log(f"CL:   {cl}")

    kernel_dir = os.path.join(ROOT, "kernel")
    c_files = sorted(glob.glob(os.path.join(kernel_dir, "*.c")))
    if not c_files:
        sys.exit("no kernel/*.c files found")
    entry_asm = os.path.join(kernel_dir, "entry.asm")
    isr_asm = os.path.join(kernel_dir, "isr.asm")
    boot_asm = os.path.join(ROOT, "boot", "boot.asm")
    entry_obj = os.path.join(BUILD, "entry.obj")
    isr_obj = os.path.join(BUILD, "isr.obj")
    kernel_pe = os.path.join(BUILD, "kernel.pe")
    kernel_bin = os.path.join(BUILD, "kernel.bin")
    boot_bin = os.path.join(BUILD, "boot.bin")
    img = os.path.join(ROOT, "myos.img")

    # 1. kernel C files -> COFF objects
    c_objs = []
    for src in c_files:
        obj = os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".obj")
        run([cl, "/nologo", "/c", "/GS-", "/Os", "/W3", "/Fo" + obj, src])
        c_objs.append(obj)
    log(f"{len(c_objs)} C objects built")

    # 2. asm stubs -> COFF (entry linked FIRST so the image starts there)
    run([nasm, "-f", "win32", entry_asm, "-o", entry_obj])
    run([nasm, "-f", "win32", isr_asm, "-o", isr_obj])
    log("entry.obj + isr.obj built")

    # 3. link a fixed-base PE (/ENTRY:entry — link prepends _ for x86;
    #    /ALIGN:16 packs sections so the image fits a single-track read)
    run([link, "/NOLOGO", "/SUBSYSTEM:NATIVE", "/ENTRY:entry",
         "/BASE:0x10000", "/ALIGN:16", "/NODEFAULTLIB",
         "/OUT:" + kernel_pe, entry_obj, isr_obj] + c_objs)
    log("kernel.pe linked")

    # 4. PE -> flat binary, learn the entry address
    pe = subprocess.run([py, os.path.join(ROOT, "tools", "pe2bin.py"),
                         kernel_pe, kernel_bin],
                        capture_output=True, text=True)
    print(pe.stdout, end="")
    if pe.returncode != 0:
        print(pe.stderr, end="")
        sys.exit(1)
    m = re.search(r"^ENTRY=(0x[0-9A-Fa-f]+)\s*$", pe.stdout, re.M)
    if not m:
        sys.exit("pe2bin.py did not report ENTRY=")
    entry = m.group(1)
    log(f"kernel entry: {entry}")

    # 5. boot sector, with the real entry address
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}", boot_asm, "-o", boot_bin])

    # 6. floppy image (all size/signature checks live in mkimg.py)
    run([py, os.path.join(ROOT, "tools", "mkimg.py"),
         boot_bin, kernel_bin, img, boot_asm])

    mode = sys.argv[1] if len(sys.argv) > 1 else "build"
    disk = [qemu, "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-boot", "order=a", "-no-reboot"]
    if mode == "run":
        log("Booting MyOS in QEMU (close the window to return)...")
        subprocess.run(disk)
    elif mode == "headless":
        serial = os.path.join(BUILD, "serial.log")
        if os.path.exists(serial):
            os.remove(serial)
        proc = subprocess.Popen(disk + ["-display", "none",
                                        "-serial", f"file:{serial}"])
        try:
            proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        time.sleep(0.5)
        if not os.path.exists(serial):
            sys.exit("no serial output captured")
        with open(serial, errors="replace") as f:
            content = f.read()
        log("--- serial log ---")
        log(content)
        # Commit-by-commit boot proof: every marker below must be present.
        expected = [
            "MYOS-BOOT-OK",  # kernel_main reached
            "IDT-OK",        # 48 IDT gates loaded
            "EXC-03-BP",     # INT3 self-test dispatched through the IDT
            "INT3-RESUMED",  # handler returned, execution resumed
        ]
        missing = [m for m in expected if m not in content]
        if not missing:
            log("HEADLESS CHECK PASSED: " + ", ".join(expected))
        else:
            sys.exit("HEADLESS CHECK FAILED: missing markers: " + ", ".join(missing))
    elif mode != "build":
        sys.exit(f"unknown mode: {mode} (use: build | run | headless)")


if __name__ == "__main__":
    main()
