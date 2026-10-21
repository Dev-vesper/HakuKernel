#!/usr/bin/env python3
"""Haku build — one straightforward script, no shell-quoting games.

Usage:
    python build.py                 build haku.img only (data image preserved)
    python build.py run             build + boot in a QEMU window
    python build.py run --fresh     recreate the FAT12 data image, then boot
    python build.py headless        fresh data image + boot headless, verify
    python build.py inspect ...     examine a FAT12 image (see --help there)

Pipeline:
    arch/x86/user/{user,child,fault,exec_target}.asm --nasm--> ELF files
        --mkfat12--> haku_fs.img (fresh only)
    kernel/**/*.c --cl--> kernel objects
    + arch/x86/kernel/{entry,isr}.asm --nasm--> entry objects
    kernel objects + entry objects --link--> kernel.pe --pe2bin--> kernel.bin
    arch/x86/boot/stage1.asm --nasm--> stage1.bin (512 B)
    arch/x86/boot/stage2.asm --nasm (kernel entry + size from PE)--> stage2.bin
    stage1 + stage2 + kernel.bin --mkimg--> haku.img (boot floppy)
    QEMU boots haku.img and attaches haku_fs.img as primary-master ATA disk,
    plus a generated spare disk on the secondary channel for device scanning

The data image (build/haku_fs.img) is user data: an existing image is
reused as-is so files from previous QEMU sessions survive the build, and
it is recreated only when missing or with --fresh. headless always starts
from a fresh image — its fault-injection tests need pristine state — and
verifies the disk again after QEMU exits (tools/lsfat12.py --check).

All tools are invoked with argument lists (never through cmd.exe), so paths
with spaces just work. The C compiler and linker are the only host-specific
piece: Windows uses VS2022's 32-bit cl.exe + link.exe, while other platforms
use clang-cl + lld-link. Both accept the identical MSVC-style switches
(/c, /Fo, /ENTRY, /ALIGN, /NODEFAULTLIB ...) and both emit 32-bit COFF/PE, so
only tool discovery differs.
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
    s = None
    try:
        # QEMU may need a moment to listen (Windows TCP); retry instead
        # of dropping keystrokes that user processes block on.
        for _ in range(12):
            try:
                s = socket.create_connection(("127.0.0.1", port),
                                             timeout=timeout)
                break
            except OSError:
                time.sleep(0.5)
        if s is None:
            raise OSError("monitor never listened")
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


def verify_abi_headers(include_dir):
    header = os.path.join(include_dir, "haku", "abi.h")
    nasm_include = os.path.join(include_dir, "haku", "abi.inc")
    c_pattern = re.compile(
        r"^\s*#define\s+(HAKU_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\s*(?:/\*.*\*/)?\s*$")
    nasm_pattern = re.compile(
        r"^\s*%define\s+(HAKU_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*(?:;.*)?\s*$")

    def read_definitions(path, pattern):
        values = {}
        with open(path, encoding="ascii") as source:
            for line in source:
                match = pattern.match(line)
                if match:
                    values[match.group(1)] = int(match.group(2), 0)
        return values

    c_values = read_definitions(header, c_pattern)
    nasm_values = read_definitions(nasm_include, nasm_pattern)
    if not c_values or c_values != nasm_values:
        sys.exit("C and NASM ABI headers are missing or out of sync")


def find_msvc():
    """Find an x86-hosted cl.exe from any VS 2022+ installation.

    CI images may expose cl through PATH (developer prompt) or only through
    the Visual Studio directories, so check both. Prefer an x86 target
    toolset: an x64 cl.exe cannot produce the 32-bit COFF objects we link.
    """
    found = shutil.which("cl")
    if found and re.search(r"[\\/]Hostx(?:64|86)[\\/]x86[\\/]cl\.exe$",
                           found, re.IGNORECASE):
        return found
    patterns = [
        r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC"
        r"\*\bin\Hostx64\x86\cl.exe",
        r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC"
        r"\*\bin\Hostx64\x86\cl.exe",
    ]
    cands = []
    for pattern in patterns:
        cands += glob.glob(pattern)

    def version(path):
        match = re.search(r"[\\/]MSVC[\\/]([\d.]+)[\\/]bin[\\/]", path)
        return tuple(int(part) for part in match.group(1).split(".")) \
            if match else ()

    if not cands:
        sys.exit('x86 cl.exe not found: install VS2022 '
                 '"Desktop development with C++"')
    return max(cands, key=version)


def _latest(paths):
    """Pick the highest-numbered tool; distros ship clang-cl-19, lld-link-19."""
    def version(path):
        nums = re.findall(r"\d+", os.path.basename(path))
        return int(nums[-1]) if nums else -1
    return max(paths, key=version)


def find_clang_cl():
    """Locate a cl-driver clang: clang-cl, a versioned one, or clang in cl mode."""
    direct = shutil.which("clang-cl")
    if direct:
        return [direct]
    cands = []
    for d in ("/usr/lib/llvm-*/bin", "/usr/bin", "/usr/local/bin"):
        cands += glob.glob(os.path.join(d, "clang-cl-*"))
        cands += glob.glob(os.path.join(d, "clang-cl"))
    if cands:
        return [_latest(cands)]
    clang = shutil.which("clang")
    if clang:
        return [clang, "--driver-mode=cl"]
    sys.exit("clang-cl not found: install Clang (e.g. apt install clang lld)")


def find_lld_link():
    """Locate lld-link (or a versioned lld-link-19)."""
    direct = shutil.which("lld-link")
    if direct:
        return [direct]
    cands = []
    for d in ("/usr/lib/llvm-*/bin", "/usr/bin", "/usr/local/bin"):
        cands += glob.glob(os.path.join(d, "lld-link-*"))
        cands += glob.glob(os.path.join(d, "lld-link"))
    if not cands:
        sys.exit("lld-link not found: install LLVM lld (e.g. apt install lld)")
    return [_latest(cands)]


def detect_toolchain():
    """Return (cc, linker, label): a compiler+linker that share MSVC switches.

    Windows keeps VS2022's 32-bit cl.exe + link.exe. Everywhere else MSVC is
    unavailable, so clang-cl + lld-link drive the same /c, /Fo, /ENTRY, /ALIGN
    command lines targeting i686 COFF, keeping NASM win32 objects, the linked
    PE, and pe2bin's output identical to a Windows build.
    """
    if os.name == "nt":
        cl = find_msvc()
        link = os.path.join(os.path.dirname(cl), "link.exe")
        if not os.path.isfile(link):
            sys.exit(f"link.exe not found next to cl.exe: {cl}")
        return [cl], [link], "MSVC cl.exe/link.exe"
    cc = find_clang_cl() + ["--target=i686-pc-windows-msvc"]
    return cc, find_lld_link(), "clang-cl/lld-link (i686-pc-windows-msvc)"


def run(cmd):
    log("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def ppm_has_color(path, rgb):
    """True when a binary PPM (P6) contains the given RGB triple."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return False
    fields = []
    index = 2
    while len(fields) < 3 and index < len(data):
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while index < len(data) and data[index:index + 1] != b"\n":
                index += 1
            continue
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        fields.append(int(data[start:index]))
    if len(fields) < 3:
        return False
    width, height, _maxval = fields
    pixels = data[index + 1:]
    red, green, blue = rgb
    for offset in range(0, min(len(pixels), width * height * 3) - 2, 3):
        if (pixels[offset] == red and pixels[offset + 1] == green
                and pixels[offset + 2] == blue):
            return True
    return False


def main():
    nasm = find_tool("nasm", [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), r"bin\NASM\nasm.exe"),
        r"C:\Program Files\NASM\nasm.exe",
    ], "NASM  [winget install NASM.NASM]")
    cc, linker, toolchain = detect_toolchain()
    py = sys.executable
    qemu = find_tool("qemu-system-i386",
                     [r"C:\Program Files\qemu\qemu-system-i386.exe"], "QEMU")
    log(f"NASM: {nasm}")
    log(f"CC:   {' '.join(cc)}")
    log(f"LINK: {' '.join(linker)}")
    log(f"toolchain: {toolchain}")

    raw_args = sys.argv[1:]
    positionals = [a for a in raw_args if not a.startswith("-")]
    mode = positionals[0] if positionals else "build"

    if mode == "inspect":
        # Read-only forensics: needs no toolchain, never builds anything.
        rest = [a for a in raw_args if a != "inspect"]
        if not any(a.lower().endswith(".img") for a in rest):
            rest = [os.path.join(BUILD, "haku_fs.img")] + rest
        rc = subprocess.run([sys.executable,
                             os.path.join(ROOT, "tools", "lsfat12.py"),
                             ] + rest).returncode
        sys.exit(rc)

    for flag in raw_args:
        if flag.startswith("-") and flag != "--fresh":
            sys.exit(f"unknown flag: {flag} (only --fresh is supported)")
    fresh = "--fresh" in raw_args
    if len(positionals) > 1:
        sys.exit(f"unexpected argument: {positionals[1]}")

    kernel_dir = os.path.join(ROOT, "kernel")
    arch_dir = os.path.join(kernel_dir, "arch", "x86")
    include_dir = os.path.join(ROOT, "include")
    verify_abi_headers(include_dir)
    c_files = sorted(glob.glob(os.path.join(kernel_dir, "**", "*.c"),
                                recursive=True))
    if not c_files:
        sys.exit("no kernel/**/*.c files found")
    entry_asm = os.path.join(arch_dir, "kernel", "entry.asm")
    isr_asm = os.path.join(arch_dir, "kernel", "isr.asm")
    user_asm = os.path.join(arch_dir, "user", "user.asm")
    exec_target_asm = os.path.join(arch_dir, "user", "exec_target.asm")
    child_asm = os.path.join(arch_dir, "user", "child.asm")
    vfork_asm = os.path.join(arch_dir, "user", "vfork_target.asm")
    fault_asm = os.path.join(arch_dir, "user", "fault.asm")
    boot_dir = os.path.join(arch_dir, "boot")
    stage1_asm = os.path.join(boot_dir, "stage1.asm")
    stage2_asm = os.path.join(boot_dir, "stage2.asm")
    # Every kernel header directory is an include path so flat
    # #include "foo.h" directives keep working across subdirectories
    # (Linux-style ccflags; header names are unique across the tree).
    kernel_header_dirs = [
        os.path.join(arch_dir, "kernel"),
        os.path.join(arch_dir, "mm"),
        os.path.join(arch_dir, "elf"),
        os.path.join(kernel_dir, "drivers"),
        os.path.join(kernel_dir, "fs", "fat12"),
        os.path.join(kernel_dir, "fs", "vfs"),
        os.path.join(kernel_dir, "fs", "test"),
        os.path.join(kernel_dir, "kernel"),
    ]
    fat12_script = os.path.join(ROOT, "tools", "mkfat12.py")
    entry_obj = os.path.join(BUILD, "entry.obj")
    isr_obj = os.path.join(BUILD, "isr.obj")
    user_elf = os.path.join(BUILD, "user.elf")
    exec_target_elf = os.path.join(BUILD, "exec_target.elf")
    child_elf = os.path.join(BUILD, "child.elf")
    vfork_elf = os.path.join(BUILD, "vfork_target.elf")
    fault_elf = os.path.join(BUILD, "fault.elf")
    user_lib_dir = os.path.join(ROOT, "user", "lib")
    user_lib_sources = sorted(glob.glob(os.path.join(user_lib_dir, "*.c")))
    user_test_source = os.path.join(ROOT, "user", "tests", "libtest.c")
    user_start_asm = os.path.join(user_lib_dir, "crt0.asm")
    user_syscall_asm = os.path.join(user_lib_dir, "syscall.asm")
    libtest_pe = os.path.join(BUILD, "libtest.exe")
    libtest_elf = os.path.join(BUILD, "lib.elf")
    pe2elf_script = os.path.join(ROOT, "tools", "pe2elf.py")
    filesystem_image = os.path.join(BUILD, "haku_fs.img")
    spare_image = os.path.join(BUILD, "ata_spare.img")
    kernel_pe = os.path.join(BUILD, "kernel.pe")
    kernel_bin = os.path.join(BUILD, "kernel.bin")
    stage1_bin = os.path.join(BUILD, "stage1.bin")
    stage2_bin = os.path.join(BUILD, "stage2.bin")
    img = os.path.join(ROOT, "haku.img")

    # 1. User ELFs are always rebuilt (build artifacts), but the FAT12 data
    #    disk is user data: an existing image is reused untouched so files
    #    from previous QEMU sessions survive, and mkfat12 runs only for an
    #    initial image or an explicit --fresh. headless always recreates —
    #    its fault-injection tests require pristine, deterministic state.
    nasm_include = include_dir + os.sep
    run([nasm, "-I" + nasm_include, "-f", "bin", user_asm, "-o", user_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", exec_target_asm,
         "-o", exec_target_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", child_asm, "-o", child_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", vfork_asm, "-o", vfork_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", fault_asm, "-o", fault_elf])

    # The user C library is linked into a fixed-base PE image, then repacked
    # as ELF32 so the existing loader can map RX/R/RW sections.
    user_objects = []
    for source in (user_start_asm, user_syscall_asm):
        obj = os.path.join(BUILD, "uasm_" +
                           os.path.splitext(os.path.basename(source))[0] + ".obj")
        run([nasm, "-I" + include_dir, "-f", "win32", source, "-o", obj])
        user_objects.append(obj)
    for source in user_lib_sources:
        obj = os.path.join(BUILD, "ulib_" +
                           os.path.splitext(os.path.basename(source))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/O2", "/Oi-", "/W3",
                  "/I" + include_dir, "/Fo" + obj, source])
        user_objects.append(obj)
    user_test_obj = os.path.join(BUILD, "ulib_libtest.obj")
    run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/Od", "/Oi-", "/W3",
              "/I" + include_dir, "/Fo" + user_test_obj, user_test_source])
    user_objects.append(user_test_obj)
    run(linker + ["/nologo", "/MACHINE:X86", "/SUBSYSTEM:CONSOLE",
                  "/ENTRY:start", "/BASE:0x40000000", "/FIXED",
                  "/DYNAMICBASE:NO", "/NXCOMPAT:NO", "/SAFESEH:NO",
                  "/ALIGN:4096", "/FILEALIGN:512", "/NODEFAULTLIB",
                  "/OUT:" + libtest_pe] + user_objects)
    run([py, pe2elf_script, libtest_pe, libtest_elf])

    if mode == "headless" or fresh or not os.path.isfile(filesystem_image):
        if mode == "headless" and os.path.isfile(filesystem_image):
            log("headless: recreating the data image "
                "(fault-injection tests mutate the disk)")
        run([py, fat12_script, user_elf, filesystem_image,
             f"NEXT.ELF={exec_target_elf}", f"CHILD.ELF={child_elf}",
             f"FAULT.ELF={fault_elf}", f"LIB.ELF={libtest_elf}",
             f"VFORK.ELF={vfork_elf}"])
        log(f"FAT12 program disk ready "
            f"({os.path.getsize(filesystem_image)} bytes)")
    else:
        log(f"reusing data image {filesystem_image} "
            f"({os.path.getsize(filesystem_image)} bytes, "
            f"user files preserved; --fresh recreates it)")
        source_mtime = max(os.path.getmtime(user_asm),
                           os.path.getmtime(exec_target_asm),
                           os.path.getmtime(child_asm),
                           os.path.getmtime(vfork_asm),
                           os.path.getmtime(fault_asm),
                           os.path.getmtime(user_test_source),
                           max(os.path.getmtime(source)
                               for source in user_lib_sources))
        if source_mtime > os.path.getmtime(filesystem_image):
            log(f"WARNING: {os.path.basename(user_asm)} is newer than the data image - "
                "test ELFs on disk may be stale; rerun with --fresh")

    # 2. kernel C files -> COFF objects
    c_objs = []
    for src in c_files:
        obj = os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Os", "/W3",
                  "/I" + include_dir]
            + ["/I" + d for d in kernel_header_dirs]
            + ["/Fo" + obj, src])
        c_objs.append(obj)
    log(f"{len(c_objs)} C objects built")

    # 3. asm stubs -> COFF (entry linked FIRST so the image starts there)
    run([nasm, "-I" + nasm_include, "-f", "win32", entry_asm, "-o", entry_obj])
    run([nasm, "-I" + nasm_include, "-f", "win32", isr_asm, "-o", isr_obj])
    log("entry.obj + isr.obj built")

    # 4. link a fixed-base PE (/ENTRY:entry — link prepends _ for x86;
    #    /ALIGN:16 packs sections; /FIXED drops .reloc — we always load
    #    at the linked base, so runtime relocation is meaningless)
    run(linker + ["/NOLOGO", "/SUBSYSTEM:NATIVE", "/ENTRY:entry",
         "/BASE:0x10000", "/ALIGN:16", "/FIXED", "/NODEFAULTLIB",
         "/MAP:" + os.path.join(BUILD, "kernel.map"),
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

    # 5. boot stages (stage2 gets the real entry address)
    run([nasm, "-f", "bin", stage1_asm, "-o", stage1_bin])
    kernel_sectors = (os.path.getsize(kernel_bin) + 511) // 512
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}",
         f"-DKERNEL_SECTORS={kernel_sectors}", stage2_asm, "-o", stage2_bin])

    # 6. floppy image (all size/signature/sector checks live in mkimg.py)
    run([py, os.path.join(ROOT, "tools", "mkimg.py"),
         stage1_bin, stage2_bin, kernel_bin, str(kernel_sectors), img,
         stage1_asm, stage2_asm])

    with open(spare_image, "wb") as f:
        f.truncate(1474560)

    disk = [qemu, "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-drive", f"file={filesystem_image},format=raw,if=ide,index=0,media=disk",
            "-drive", f"file={spare_image},format=raw,if=ide,index=2,media=disk",
            "-boot", "order=a", "-no-reboot"]
    # CPU exception trace: shows every #GP/#PF/triple-fault with EIP.
    trace = ["-d", "int,cpu_reset", "-D", os.path.join(BUILD, "qemu.log")]
    if mode == "run":
        log("Booting Haku in QEMU (close the window to return)...")
        subprocess.run(disk)
    elif mode == "headless":
        serial = os.path.join(BUILD, "serial.log")
        shot = os.path.join(BUILD, "screen.png")
        ppm = os.path.join(BUILD, "screen.ppm")
        if os.path.exists(serial):
            os.remove(serial)
        # Human monitor on TCP (for screendump now, sendkey later).
        MON_PORT = 5555
        proc = subprocess.Popen(disk + trace + ["-display", "none",
                                                 "-serial", f"file:{serial}",
                                        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server=on,wait=off"])
        try:
            # The initial test process blocks once in read(0, ..., 1).
            # CI runners are slower; allow the environment to delay the key
            # and to extend the wait without changing local behavior.
            time.sleep(float(os.environ.get("HAKU_BOOT_DELAY", "1.0")))
            monitor_cmd(MON_PORT, ["sendkey x"])
            proc.wait(timeout=float(
                os.environ.get("HAKU_HEADLESS_TIMEOUT", "30")))
        except subprocess.TimeoutExpired:
            # Still alive: interrogate the CPU state, grab a screenshot, stop it.
            regs = monitor_cmd(MON_PORT, ["info registers", f"screendump {shot}",
                                              f"screendump {ppm}",
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
            "KLOG-LEVELS-OK",# leveled logging gates DEBUG at the INFO threshold
            "PAGING-OK",     # 8 MiB identity map, CR0.PG on, R/W probe passed
            "HEAP-OK",       # kmalloc/kfree/krealloc self-test passed
            "E820-OK",       # stage2 INT 15h map handoff verified
            "FRAME-OK",      # frame_alloc/free self-test passed
            "GDT-OK",        # kernel/user selectors and ring-0 TSS stack loaded
            "ELF-REJECT-OK", # malformed images and W+X segments were rejected
            "ELF-LOAD-OK",   # valid ELF32 PT_LOAD segments loaded to fresh CR3
            "USERMODE-READY",# user stack mapped in its own address space
            "ATA-OK primary-master LBA28",
            "ATA-SCAN-OK devices=2",
            "secondary-master LBA28", # spare disk on the second IDE channel
            "ATA-RECOVERY-OK",   # software reset + reread after a recovery
            "ATA-MULTI-OK",     # second channel device answers sector reads
            "FAT12-MOUNT-OK",
            "FAT12-READ-OK name=HAKU.ELF",
            "USERMODE-ENTER",# iret entered CPL 3
            "SCHED-READY tasks=1 kernel-stack=per-task",
            "SCHED-START pid=1",
            "SCHED-TIMER-OK",
            "SCHED-WAITQ-OK", # blocked read left the run queue; idle ran instead
            "USERMODE-OK",   # ELF32 user image executed POSIX-style syscalls
            "USER-SYSCALL-CHECKS-OK", # EFAULT/EBADF/EINVAL/ENOSYS + flag/fd/whence paths passed
            "USER-READ-OK",  # blocking read copied keyboard input to user memory
            "USER-FILE-IO-OK", # FAT12 create/write/read/seek/close passed
            "USER-FILE-OPS-OK", # unlink/rename/stat/fstat, open-file removed/renamed
            "USER-DUP-OK",   # dup/dup2 share one open file description
            "STDIO-OK",      # standard descriptors 0/1/2 installed on the console
            "USER-STDIO-OK", # stdio redirect via dup2, close, and restore
            "SPAWN-CHILD-RAN",
            "USER-FORK-OK",   # fork/wait4/exit_group round trip
            "USER-COW-OK",    # forked pages are private once either side writes
            "USER-VFORK-OK",  # vfork shares the space; parent wakes on exit/exec
            "VFORK-EXEC-RAN",
            "USER-WAITPID-OK child status=42 reaped",
            "USER-FD-INHERIT-OK", # spawn inherited fd 3; child exit closed it
            "USER-TIME-OK",  # gettimeofday reads ticks; nanosleep blocks until wake
            "USER-MEM-OK",   # brk grows/shrinks; mmap/munmap allocate and release pages
            "USER-DIR-OK",   # subdirectories, paths, and "."/".." resolution
            "USER-USAGE-OK", # getusage reports per-process memory and CPU counters
            "USER-PROT-OK",  # RX/read-only stores, unmapped reads, and freed pages fault
            "CRASH-REPORT-OK", # faulted user frame dumped with registers and CRs
            "EXECVE-OK pid=1", # exec replaced the image and preserved descriptors
            "USER-EXEC-TARGET-OK", # argv/envp stack and image replacement passed
            "USER-LIB-OK",   # C library syscalls/strings/malloc/file I/O
            "USER-EXIT-OK status=0 pid=1",
            "SCHED-REAP-OK",
            "FS-ERRORS-OK",  # missing/invalid/exists/badfd map to distinct errno
            "FS-EROFS-OK",   # read-only mount rejects every mutating path
            "FS-FILE-EDGE-OK", # empty files, exact-sector, span, partial writes
            "FS-CORRUPT-OK", # cut/bad/reserved/cyclic chains + data-full ENOSPC
            "FS-ATA-OK",     # ATA bounds/null transfers rejected
            "FS-STAT-OK",    # stat/fstat metadata: mode/size/nlink/blocks
            "FS-ROLLBACK-OK", # failed writes roll back size/clusters
            "FS-ENOSPC-OK",  # full root directory refused with ENOSPC
            "PIC-OK",        # 8259 remapped to 0x20/0x28
            "PIT-OK",        # 100 Hz timer programmed
            "KBD-OK",        # PS/2 keyboard IRQ1 handler installed
            "TERMINAL-OK",   # terminal/display abstraction selected a backend
            "T+1s",          # hardware timer IRQs arriving
            "T+2s",
            "T+3s",
        ]
        missing = [m for m in expected if m not in content]
        if "EXECVE-FAIL" in content:
            missing.append("successful execve did not return an error")
        if "KERNEL PANIC" in content:
            missing.append("kernel panicked during the headless run")
        # The framebuffer console proves itself on the screen: when the
        # graphics mode is live the signature block the driver draws must
        # show up in the captured framebuffer.
        if "FB-OK" in content:
            if not os.path.exists(ppm):
                missing.append("framebuffer active but no screendump captured")
            elif not ppm_has_color(ppm, (0x20, 0xA0, 0xFF)):
                missing.append("framebuffer signature color missing from the"
                               " screendump")
        elif "FB-SKIP" not in content:
            missing.append("no framebuffer status line in the serial log")
        unexpected = [m for m in ("USER-SYSCALL-FAIL",
                                     "FS-ERROR-FAIL",
                                     "FS-EDGE-FAIL",
                                     "FS-CORRUPT-FAIL",
                                     "FS-ATA-FAIL",
                                     "FS-ROLLBACK-FAIL",
                                     "FS-ENOSPC-FAIL") if m in content]
        if not missing and not unexpected:
            log("HEADLESS CHECK PASSED: " + ", ".join(expected))
            # The guest just ran fault-injection restores and a root-fill
            # against this image: prove on the host side that mirrors,
            # chains, and allocation state are still consistent.
            log("--- post-run filesystem inspection ---")
            chk = subprocess.run(
                [py, os.path.join(ROOT, "tools", "lsfat12.py"),
                 "--check", filesystem_image],
                capture_output=True, text=True)
            print(chk.stdout, end="")
            if chk.returncode != 0:
                print(chk.stderr, end="")
                sys.exit("HEADLESS CHECK FAILED: "
                         "data image inconsistent after run")
        else:
            failures = []
            if missing:
                failures.append("missing markers: " + ", ".join(missing))
            if unexpected:
                failures.append("unexpected markers: " + ", ".join(unexpected))
            log("hint: inspect the disk with "
                "`python build.py inspect [--check] [IMAGE.img]`")
            sys.exit("HEADLESS CHECK FAILED: " + "; ".join(failures))
    elif mode != "build":
        sys.exit(f"unknown mode: {mode} "
                 f"(use: build | run [--fresh] | headless | inspect [...])")


if __name__ == "__main__":
    main()
