#!/usr/bin/env python3
"""Haku v0.1 build — one straightforward script, no shell-quoting games.

Usage:
    python build.py            build haku.img only
    python build.py run        build + boot in a QEMU window
    python build.py headless   build + boot headless, verify via serial log

Pipeline:
    kernel.c --cl--> kernel.obj  +  entry.asm --nasm--> entry.obj
    entry.obj + kernel.obj --link--> kernel.pe --pe2bin--> kernel.bin
    stage1.asm --nasm--> stage1.bin (512 B)
    stage2.asm --nasm (-DKERNEL_ENTRY from the PE)--> stage2.bin
    stage1 + stage2 + kernel.bin --mkimg--> haku.img (1.44 MB floppy)

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


def monitor_cmd(port, cmds, timeout=5):
    """Run HMP commands over the TCP monitor, return the raw transcript."""
    import socket
    out = []
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.settimeout(timeout)
        try:
            out.append(s.recv(65536))
        except OSError:
            pass
        for cmd in cmds:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(0.7)
            try:
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    out.append(chunk)
                    if b"(qemu)" in chunk:
                        break
            except OSError:
                pass
        s.close()
    except OSError as e:
        log(f"(monitor unavailable: {e})")
    return b"".join(out)


def take_screenshot(port, path):
    """Ask QEMU's human monitor for a PPM screenshot (best effort)."""
    blob = monitor_cmd(port, [f"screendump {path}"])
    time.sleep(1.0)
    if os.path.exists(path):
        log(f"screenshot: {path}")
    elif blob:
        log(f"(screendump produced no file; monitor said: {blob[-200:]!r})")


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
    user_asm = os.path.join(kernel_dir, "user.asm")
    boot_dir = os.path.join(ROOT, "boot")
    stage1_asm = os.path.join(boot_dir, "stage1.asm")
    stage2_asm = os.path.join(boot_dir, "stage2.asm")
    entry_obj = os.path.join(BUILD, "entry.obj")
    isr_obj = os.path.join(BUILD, "isr.obj")
    user_obj = os.path.join(BUILD, "user.obj")
    kernel_pe = os.path.join(BUILD, "kernel.pe")
    kernel_bin = os.path.join(BUILD, "kernel.bin")
    stage1_bin = os.path.join(BUILD, "stage1.bin")
    stage2_bin = os.path.join(BUILD, "stage2.bin")
    img = os.path.join(ROOT, "haku.img")

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
    run([nasm, "-f", "win32", user_asm, "-o", user_obj])
    log("entry.obj + isr.obj + user.obj built")

    # 3. link a fixed-base PE (/ENTRY:entry — link prepends _ for x86;
    #    /ALIGN:16 packs sections; /FIXED drops .reloc — we always load
    #    at the linked base, so runtime relocation is meaningless)
    run([link, "/NOLOGO", "/SUBSYSTEM:NATIVE", "/ENTRY:entry",
         "/BASE:0x10000", "/ALIGN:16", "/FIXED", "/NODEFAULTLIB",
         "/MAP:" + os.path.join(BUILD, "kernel.map"),
         "/OUT:" + kernel_pe, entry_obj, isr_obj, user_obj] + c_objs)
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

    # 5. boot stages (stage2 gets the real entry address)
    run([nasm, "-f", "bin", stage1_asm, "-o", stage1_bin])
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}", stage2_asm, "-o", stage2_bin])

    # 6. floppy image (all size/signature/sector checks live in mkimg.py)
    run([py, os.path.join(ROOT, "tools", "mkimg.py"),
         stage1_bin, stage2_bin, kernel_bin, img, stage1_asm, stage2_asm])

    mode = sys.argv[1] if len(sys.argv) > 1 else "build"
    disk = [qemu, "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-boot", "order=a", "-no-reboot"]
    # CPU exception trace: shows every #GP/#PF/triple-fault with EIP.
    trace = ["-d", "int,cpu_reset", "-D", os.path.join(BUILD, "qemu.log")]
    if mode == "run":
        log("Booting Haku in QEMU (close the window to return)...")
        subprocess.run(disk)
    elif mode == "headless":
        serial = os.path.join(BUILD, "serial.log")
        shot = os.path.join(BUILD, "screen.png")
        if os.path.exists(serial):
            os.remove(serial)
        # Human monitor on TCP (for screendump now, sendkey later).
        MON_PORT = 5555
        proc = subprocess.Popen(disk + trace + ["-display", "none",
                                                 "-serial", f"file:{serial}",
                                        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server=on,wait=off"])
        try:
            # The ring-3 syscall test blocks in read(0, ..., 1).
            time.sleep(1.0)
            monitor_cmd(MON_PORT, ["sendkey a"])
            proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            # Still alive: interrogate the CPU state, grab a screenshot, stop it.
            regs = monitor_cmd(MON_PORT, ["info registers", f"screendump {shot}",
                                              "xp /3wx 0x500",
                                              "xp /64wx 0x11240",
                                              "xp /64wx 0x11980"])
            with open(os.path.join(BUILD, "regs.txt"), "wb") as f:
                f.write(regs)
            for line in regs.decode("utf-8", "replace").splitlines():
                if line.startswith(("EIP", "EAX", "CS ", "CR0", "EFL")):
                    log("  " + line.strip())
            if os.path.exists(shot):
                log(f"screenshot: {shot}")
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
            "HAKU-BOOT-OK",  # kernel_main reached
            "IDT-OK",        # 48 IDT gates loaded
            "EXC-03-BP",     # INT3 self-test dispatched through the IDT
            "INT3-RESUMED",  # handler returned, execution resumed
            "PAGING-OK",     # 8 MiB identity map, CR0.PG on, R/W probe passed
            "HEAP-OK",       # kmalloc/kfree/krealloc self-test passed
            "E820-OK",       # stage2 INT 15h map handoff verified
            "FRAME-OK",      # frame_alloc/free self-test passed
            "GDT-OK",        # kernel/user selectors and ring-0 TSS stack loaded
            "USERMODE-READY",# user text + stack pages mapped user-accessible
            "USERMODE-ENTER",# iret entered CPL 3
            "USERMODE-OK",   # POSIX-style write/getpid syscalls passed
            "USER-SYSCALL-CHECKS-OK", # EFAULT/EBADF/EINVAL/ENOSYS paths passed
            "USER-READ-OK",  # blocking read copied keyboard input to user memory
            "USER-EXIT-OK status=0",
            "USERMODE-RETURNED",
            "PIC-OK",        # 8259 remapped to 0x20/0x28
            "PIT-OK",        # 100 Hz timer programmed
            "KBD-OK",        # PS/2 keyboard IRQ1 handler installed
            "T+1s",          # hardware timer IRQs arriving
            "T+2s",
            "T+3s",
        ]
        missing = [m for m in expected if m not in content]
        unexpected = [m for m in ("USER-SYSCALL-FAIL",) if m in content]
        if not missing and not unexpected:
            log("HEADLESS CHECK PASSED: " + ", ".join(expected))
        else:
            failures = []
            if missing:
                failures.append("missing markers: " + ", ".join(missing))
            if unexpected:
                failures.append("unexpected markers: " + ", ".join(unexpected))
            sys.exit("HEADLESS CHECK FAILED: " + "; ".join(failures))
    elif mode != "build":
        sys.exit(f"unknown mode: {mode} (use: build | run | headless)")


if __name__ == "__main__":
    main()
