#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run under MSYS2 UCRT64 from the repository root.
set -euo pipefail
root=$(pwd)
mkdir -p build-win64 win-release
cd build-win64
bash "$root/configure" --target-list=x86_64-softmmu --disable-docs --disable-werror --disable-rust --disable-gtk --enable-sdl --enable-slirp --disable-opengl --disable-download
ninja -j "${JOBS:-4}" qemu-system-x86_64.exe qemu-img.exe
cd "$root"
stage=win-release/Qemu-Lightning
mkdir -p "$stage/share/qemu" "$stage/licenses"
cp build-win64/{qemu-system-x86_64,qemu-img}.exe "$stage/"
cp pc-bios/{bios.bin,bios-256k.bin,efi-*.rom,kvmvapic.bin,linuxboot*.bin,vgabios*.bin,multiboot*.bin,edk2-licenses.txt} "$stage/share/qemu/"
cp -r pc-bios/keymaps "$stage/share/qemu/"
cp COPYING COPYING.LIB LICENSE LICENSE.MIT "$stage/licenses/"
for fw in edk2-x86_64-code.fd edk2-i386-vars.fd; do bzip2 -dc "pc-bios/$fw.bz2" > "$stage/share/qemu/$fw"; done
python - "$stage" <<'PY'
from pathlib import Path
import re, shutil, subprocess, sys
stage=Path(sys.argv[1])
bindir=Path(shutil.which('gcc')).parent
todo=list(stage.glob('*.exe'))
packages=set()
while todo:
    pe=todo.pop()
    raw=subprocess.check_output(['objdump','-p',str(pe)],text=True,errors='replace')
    for name in re.findall(r'DLL Name:\s*([^\s]+)',raw,re.I):
        lib=bindir/name
        if lib.is_file() and not (stage/name).exists():
            shutil.copy2(lib,stage/name)
            todo.append(stage/name)
            posix=subprocess.check_output(['cygpath','-u',str(lib)],text=True).strip()
            packages.add(subprocess.check_output(['pacman','-Qoq',posix],text=True).strip())
for pkg in sorted(packages):
    files=subprocess.check_output(['pacman','-Qlq',pkg],text=True).splitlines()
    for posix in files:
        if '/share/licenses/' in posix and not posix.endswith('/'):
            src=Path(subprocess.check_output(['cygpath','-w',posix],text=True).strip())
            out=stage/'licenses'/pkg/src.name
            out.parent.mkdir(exist_ok=True)
            shutil.copy2(src,out)
(stage/'README.txt').write_text('Extract the entire folder. In PowerShell, run .\\qemu-system-x86_64.exe -L share/qemu -m 4096 -cdrom path-to-your.iso\nUse -accel whpx if Windows Hypervisor Platform is enabled. The Venus accelerated display requires a Linux host.\n')
PY
