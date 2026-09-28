#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run the real engine argument helper on HarmonyOS, regular and hardened modes.

No NAPI/ArkUI/engine emulation or copied argument algorithm is used: the native
fixture includes the exact helper header compiled by engine_host_openharmony.cpp.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("OPENHARMONY_SDK_PATH"))
    parser.add_argument("--cxx", default=os.environ.get("CXX"))
    parser.add_argument("--sign-tool", default=shutil.which("binary-sign-tool"))
    args = parser.parse_args()
    if not args.sdk:
        parser.error("Provide --sdk or OPENHARMONY_SDK_PATH")
    sdk = Path(args.sdk).expanduser().resolve()
    if (sdk / "native" / "sysroot").is_dir():
        sdk /= "native"
    if not (sdk / "sysroot" / "usr" / "include").is_dir():
        parser.error("The SDK must provide native sysroot/usr/include")
    if not args.sign_tool:
        parser.error("Provide --sign-tool or install binary-sign-tool in PATH")
    tmpdir = os.environ.get("TMPDIR")
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error("TMPDIR must name an existing writable directory")
    cxx = args.cxx or str(sdk / "llvm" / "bin" / "clang++")
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="godot-engine-arguments-", dir=tmpdir) as temporary:
        temp = Path(temporary)
        for mode, definitions in (("regular", ["-DOVERRIDE_PATH_ENABLED"]), ("hardened", [])):
            unsigned = temp / (mode + ".unsigned")
            executable = temp / mode
            subprocess.run(
                [
                    cxx,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"--sysroot={sdk / 'sysroot'}",
                    f"-I{tests.parent}",
                    *definitions,
                    str(tests / "native_shell_fixture" / "engine_arguments.cpp"),
                    "-o",
                    str(unsigned),
                ],
                check=True,
            )
            subprocess.run(
                [args.sign_tool, "sign", "-selfSign", "1", "-inFile", str(unsigned), "-outFile", str(executable)],
                check=True,
            )
            executable.chmod(0o700)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
