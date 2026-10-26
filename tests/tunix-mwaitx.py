#!/usr/bin/env python3
"""Check native MWAITX CPU policy on a supporting AMD/KVM host (no guest image)."""
import argparse
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def check(binary, accelerator, cpu, pm, expected):
    with tempfile.TemporaryDirectory(prefix="qemu-mwaitx-") as directory:
        endpoint = Path(directory) / "qmp"
        command = [binary, "-accel", accelerator, "-cpu", cpu, "-S",
                   "-display", "none", "-nodefaults", "-machine", "pc",
                   "-qmp", f"unix:{endpoint},server=on,wait=off",
                   "-overcommit", f"cpu-pm={'on' if pm else 'off'}"]
        with tempfile.TemporaryFile(mode="w+") as errors:
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + 10
                if expected is None:
                    code = process.wait(timeout=10)
                    errors.seek(0)
                    diagnostic = errors.read()
                    assert code != 0 and "mwaitx" in diagnostic, diagnostic
                else:
                    while not endpoint.exists():
                        if process.poll() is not None or time.monotonic() > deadline:
                            errors.seek(0)
                            raise RuntimeError(errors.read() or "QMP startup timed out")
                        time.sleep(.01)
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.settimeout(10)
                        connection.connect(str(endpoint))
                        with connection.makefile("rwb") as stream:
                            json.loads(stream.readline())

                            def qmp(name, arguments=None):
                                stream.write((json.dumps({"execute": name,
                                              "arguments": arguments or {}}) + "\n").encode())
                                stream.flush()
                                while True:
                                    reply = json.loads(stream.readline())
                                    if "error" in reply:
                                        raise RuntimeError(reply)
                                    if "return" in reply:
                                        return reply["return"]

                            qmp("qmp_capabilities")
                            path = qmp("query-cpus-fast")[0]["qom-path"]
                            actual = qmp("qom-get", {"path": path, "property": "mwaitx"})
                            assert actual == expected, (command, actual, expected)
                            if actual and "invtsc=off" in cpu:
                                try:
                                    qmp("migrate", {"uri": "exec:cat >/dev/null"})
                                except RuntimeError as error:
                                    assert "mwaitx" in str(error), error
                                else:
                                    raise AssertionError("native MWAITX migration was accepted")
                            qmp("quit")
                    assert process.wait(timeout=10) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
        print(f"PASS: {accelerator} {cpu} cpu-pm={pm}: {expected}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu")
    args = parser.parse_args()
    for accelerator, cpu, pm, expected in [
        ("kvm", "host,migratable=off,enforce", True, True),
        ("kvm", "host,migratable=off,invtsc=off,enforce", True, True),
        ("kvm", "host,migratable=off,mwaitx=off,enforce", True, False),
        ("kvm", "host,migratable=off,enforce", False, False),
        ("kvm", "host,enforce", True, False),
        ("kvm", "host,migratable=off,mwaitx=on,enforce", False, None),
        ("kvm", "host,mwaitx=on,enforce", True, None),
        ("tcg", "max,migratable=off,mwaitx=on,enforce", False, None),
    ]:
        check(args.qemu, accelerator, cpu, pm, expected)
