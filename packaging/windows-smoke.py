# SPDX-License-Identifier: MIT
"""Check the native CLI, firmware startup and QMP with only packaged DLLs."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

stage = Path(sys.argv[1]).resolve()
env = dict(os.environ, PATH=os.path.join(os.environ["SystemRoot"], "System32"))
emulator = str(stage / "qemu-system-x86_64.exe")
subprocess.run([emulator, "--version"], env=env, cwd=stage, timeout=20, check=True)
with tempfile.TemporaryDirectory() as temp:
    subprocess.run([str(stage / "qemu-img.exe"), "create", "-f", "qcow2",
                    str(Path(temp) / "blank.qcow2"), "16M"], env=env, cwd=stage, timeout=20, check=True)
    # QEMU connects back to an already-bound loopback port, avoiding a port race.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(30)
        port = listener.getsockname()[1]
        proc = subprocess.Popen([emulator, "-L", "share/qemu", "-machine", "q35,accel=tcg",
                                 "-display", "none", "-nodefaults", "-S", "-qmp",
                                 f"tcp:127.0.0.1:{port}"], env=env, cwd=stage)
        try:
            with listener.accept()[0] as conn:
                conn.settimeout(15)
                with conn.makefile("rwb", buffering=0) as qmp:
                    assert "QMP" in json.loads(qmp.readline())
                    for cmd in ("qmp_capabilities", "query-status", "quit"):
                        qmp.write(json.dumps({"execute":cmd,"id":cmd}).encode()+b"\n")
                        while True:
                            response = json.loads(qmp.readline())
                            if response.get("id") == cmd:
                                assert "return" in response, response
                                if cmd == "query-status":
                                    assert response["return"]["status"] == "prelaunch", response
                                break
            if proc.wait(timeout=15):
                raise RuntimeError(f"Emulator exited with {proc.returncode}")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
