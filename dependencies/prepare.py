#!/usr/bin/env python3
"""Rebase generated local tool configuration after moving this source snapshot.

No downloads, installers, guest changes or source-tree cleanup are performed.
Only generated VFS, Cargo and development pkg-config paths are rewritten.
"""
from pathlib import Path
import json
import re


def save_if_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def prepare(root):
    root = Path(root).resolve()
    toolchain = root / 'driver-toolchain'
    overlay = toolchain / 'case-insensitive-headers.json'
    if overlay.is_file():
        data = json.loads(overlay.read_text())
        def relocate(value):
            if isinstance(value, str):
                marker = '/dependencies/driver-toolchain/'
                if marker in value:
                    return str(toolchain / value.split(marker, 1)[1])
            if isinstance(value, dict):
                return {key: relocate(item) for key, item in value.items()}
            if isinstance(value, list):
                return [relocate(item) for item in value]
            return value
        save_if_changed(overlay, json.dumps(relocate(data)) + '\n')
    cargo = root / 'cargo'
    cargo.mkdir(exist_ok=True)
    save_if_changed(cargo / 'config.toml',
        '[source.crates-io]\nreplace-with="vendored-sources"\n'
        '[source.vendored-sources]\ndirectory=' + json.dumps(str(root / 'rust-crates')) + '\n')
    # Native development packages were unpacked without their filesystem root.
    # Resolve include/library paths within that retained sysroot, not host /usr.
    for pc in (root / 'dev-sysroot/usr/lib/x86_64-linux-gnu/pkgconfig').glob('*.pc'):
        text = re.sub(r'^prefix=.*$', 'prefix=${pcfiledir}/../../..', pc.read_text(), flags=re.M)
        text = text.replace('/usr/include', '${prefix}/include').replace('/usr/lib/', '${prefix}/lib/')
        save_if_changed(pc, text)


if __name__ == '__main__':
    prepare(Path(__file__).resolve().parent)
