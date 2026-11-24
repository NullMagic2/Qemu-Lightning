#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package an assembled Linux installation tree on its target Debian-family OS."""
import argparse
import platform
import hashlib
import subprocess
import tempfile
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staging", type=Path, help="installation tree containing usr/")
    parser.add_argument("destination", type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    stage = args.staging.resolve()
    dest = args.destination.resolve()
    runtime = stage / "usr/lib/qemu-lightning"
    control = stage / "DEBIAN"
    control.mkdir(exist_ok=True)
    libraries = [runtime / "lib/lightning", runtime / "lib/lightning-venus"]
    binaries = []
    for folder in [runtime / "bin", runtime / "tools", *libraries]:
        for path in sorted(folder.iterdir()):
            if path.is_file() and not path.is_symlink():
                with path.open("rb") as f:
                    if f.read(4) == b"\x7fELF":
                        binaries.append("-e" + str(path))
    with tempfile.TemporaryDirectory() as directory:
        debian = Path(directory) / "debian"
        debian.mkdir()
        (debian / "control").write_text("Source: qemu-lightning\nSection: misc\nPriority: optional\nMaintainer: Qemu-Lightning contributors <noreply@localhost>\n\nPackage: qemu-lightning\nArchitecture: amd64\nDescription: Virtual machine manager\n")
        # These libraries are shipped together in their private runtime directory.
        (debian / "shlibs.local").write_text("".join(f"{name} {abi} qemu-lightning\n" for name, abi in [
            ("libvirglrenderer", "1"), ("libgtk-vnc-2.0", "0"), ("libgvnc-1.0", "0"), ("libgvncpulse-1.0", "0")]))
        deps = subprocess.check_output(["dpkg-shlibdeps", "-O", "-xqemu-lightning",
                                      *["-l"+str(lib) for lib in libraries], *binaries],
                                     cwd=directory, text=True).strip().removeprefix("shlibs:Depends=")
    codename = platform.freedesktop_os_release().get("UBUNTU_CODENAME")
    svg_loader = "glycin-loaders" if codename == "resolute" else "librsvg2-common"
    deps = ", ".join(sorted(set(deps.split(", ")) | {svg_loader, "libvulkan1", "ovmf", "swtpm", "xdg-utils", "xorriso"}))
    files = [f for f in (stage / "usr").rglob("*") if f.is_file() and not f.is_symlink()]
    size = (sum(f.stat().st_size for f in files) + 1023) // 1024
    (control / "control").write_text(
        f"Package: qemu-lightning\nVersion: {args.version}\nArchitecture: amd64\nInstalled-Size: {size}\n"
        "Section: otherosfs\nPriority: optional\nMaintainer: Qemu-Lightning contributors <noreply@localhost>\n"
        f"Depends: {deps}\nRecommends: mesa-vulkan-drivers\n"
        "Homepage: https://github.com/NullMagic2/Qemu-Lightning\n"
        "Description: Desktop virtual machine manager with accelerated Windows guest graphics\n"
        " Includes the emulator, GTK manager, Venus renderer, and Windows guest tools.\n")
    with (control / "md5sums").open("w") as manifest:
        for f in sorted(files):
            with f.open("rb") as data:
                manifest.write(hashlib.file_digest(data, "md5").hexdigest()+"  "+str(f.relative_to(stage))+"\n")
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["dpkg-deb","-Zxz","-z5","--root-owner-group","--build",str(stage),str(dest)],check=True)

if __name__ == "__main__":
    main()
