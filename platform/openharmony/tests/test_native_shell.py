#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Build and execute the real common NAPI binding with mocked engine boundaries.

Run on HarmonyOS with its native SDK/compiler and Node N-API implementation.
This is not an ArkUI, Godot renderer, managed runtime or signed HAP test.
All artifacts, mutable data and self-signed test binaries live in $TMPDIR.
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
    parser.add_argument("--cxx", default=os.environ.get("CXX"), help="Override native clang++ path")
    parser.add_argument("--node", default=shutil.which("node"))
    parser.add_argument("--sign-tool", default=shutil.which("binary-sign-tool"))
    args = parser.parse_args()
    if not args.sdk:
        parser.error("Provide --sdk or OPENHARMONY_SDK_PATH (SDK root or native directory)")
    sdk = Path(args.sdk).expanduser().resolve()
    if (sdk / "native" / "sysroot").is_dir():
        sdk /= "native"
    if not (sdk / "sysroot" / "usr" / "include" / "napi" / "native_api.h").is_file():
        parser.error("SDK must contain sysroot/usr/include/napi/native_api.h")
    cxx = args.cxx or str(sdk / "llvm" / "bin" / "clang++")
    if not args.node or not args.sign_tool:
        parser.error("Native Node and binary-sign-tool must be in PATH or passed explicitly")
    tmpdir = os.environ.get("TMPDIR")
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error("TMPDIR must name an existing writable directory")

    tests = Path(__file__).resolve().parent
    repo = tests.parents[2]
    cpp = repo / "misc" / "dist" / "openharmony_template" / "entry" / "src" / "main" / "cpp"
    with tempfile.TemporaryDirectory(prefix="godot-native-shell-", dir=tmpdir) as temporary:
        temp = Path(temporary)
        unsigned = temp / "entry.unsigned.node"
        signed = temp / "entry.node"
        subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-shared",
                "-fPIC",
                "-Wall",
                "-Wextra",
                f"--sysroot={sdk / 'sysroot'}",
                f"-I{repo / 'platform' / 'openharmony'}",
                str(cpp / "napi_init.cpp"),
                str(cpp / "runtime_paths.cpp"),
                str(tests / "native_shell_fixture" / "stubs.cpp"),
                "-ldl",
                "-o",
                str(unsigned),
            ],
            check=True,
        )
        subprocess.run(
            [args.sign_tool, "sign", "-selfSign", "1", "-inFile", str(unsigned), "-outFile", str(signed)],
            check=True,
        )
        environment = os.environ.copy()
        environment["GODOT_NATIVE_TEST_DIR"] = str(temp)
        environment.pop("GODOT_TEST_FAIL_START", None)
        subprocess.run(
            [args.node, str(tests / "native_shell_fixture" / "check.cjs")], env=environment, check=True
        )


if __name__ == "__main__":
    main()
