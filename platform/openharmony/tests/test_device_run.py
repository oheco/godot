#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Compile, sign and run the real standalone DeviceRunSession on HarmonyOS.

Only an in-memory Executor fixture runs: no HDC, connected target, HAP signing,
installation, port forwarding, daemon or OS network activity is performed.
Temporary sources/binaries are automatically removed from application TMPDIR.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def native_sdk(value):
    sdk = Path(value).expanduser().resolve()
    for candidate in (sdk, sdk / "native", sdk / "26.0.0" / "native"):
        if (candidate / "sysroot" / "usr" / "include").is_dir():
            return candidate.resolve()
    raise ValueError("SDK must provide native sysroot/usr/include (native package or SDK26 view)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("OPENHARMONY_SDK_PATH"))
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++"))
    parser.add_argument("--sign-tool", default=shutil.which("binary-sign-tool"))
    args = parser.parse_args()
    if not args.sdk:
        parser.error("Provide --sdk or OPENHARMONY_SDK_PATH")
    try:
        sdk = native_sdk(args.sdk)
    except ValueError as error:
        parser.error(str(error))
    if not args.cxx:
        parser.error("Provide --cxx or a native clang++ in PATH")
    if not args.sign_tool:
        parser.error("Provide --sign-tool or binary-sign-tool in PATH")
    tmpdir = os.environ.get("TMPDIR")
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error("TMPDIR must name an existing application-private writable directory")
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="godot-device-run-", dir=tmpdir) as temporary:
        temp = Path(temporary)
        unsigned = temp / "device-run.unsigned"
        executable = temp / "device-run"
        subprocess.run(
            [
                args.cxx,
                "--target=aarch64-linux-ohos",
                "-std=c++17",
                "-fno-exceptions",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"--sysroot={sdk / 'sysroot'}",
                f"-I{tests.parent}",
                str(tests / "native_device_run_fixture" / "device_run.cpp"),
                "-o",
                str(unsigned),
            ],
            check=True,
        )
        subprocess.run(
            [args.sign_tool, "sign", "-selfSign", "1", "-inFile", str(unsigned), "-outFile", str(executable)],
            check=True,
        )
        # This chmod is only for a disposable native executable on real TMPDIR,
        # never a credential or directory on HOME/hmdfs.
        executable.chmod(0o700)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
