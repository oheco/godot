#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Compile/sign/run the actual OpenHarmony direct-argv process helper natively.

Fixtures, binaries and sentinels live only under TMPDIR and are automatically
removed. The runner owns a fresh process group and, when available, acts as a
subreaper so timeout cleanup cannot leave fixture descendants. No shell command
execution, personal keys, SDK changes, HAP installation or publishing occurs.
The 64 MiB output limit is not stress-tested; the deadlock test emits 4 MiB.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


def native_sdk(path):
    root = Path(path).expanduser().resolve()
    candidates = [root, root / "native", root / "26.0.0/native", root / "root/26.0.0/native"]
    for parent in (root, root / "root"):
        if parent.is_dir():
            candidates.extend(parent.glob("*/native"))
    for candidate in candidates:
        if (candidate / "sysroot/usr/include").is_dir() and (candidate / "llvm/bin/clang++").is_file():
            return candidate.resolve()
    raise ValueError("SDK must contain native sysroot/llvm, or be an oo SDK view with a native component")


def enable_subreaper():
    """Return a restore callback, or None if the platform does not support it."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        value = ctypes.c_int()
        if libc.prctl(37, ctypes.byref(value), 0, 0, 0) != 0:  # PR_GET_CHILD_SUBREAPER
            return None
        if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
            return None
        old = value.value
        return lambda: libc.prctl(36, old, 0, 0, 0)
    except (AttributeError, OSError):
        return None


def cleanup_owned_group(group):
    # Only act on a group containing our own waitable/adopted descendants. This
    # avoids blindly signaling a numeric PGID after the direct fixture is reaped.
    while True:
        try:
            child, _ = os.waitpid(-group, os.WNOHANG)
        except ChildProcessError:
            return
        if child:
            continue
        try:
            os.killpg(group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        while True:
            try:
                os.waitpid(-group, 0)
            except ChildProcessError:
                return
            except InterruptedError:
                continue


def run_fixture(command, environment, timeout):
    process = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        return process.returncode, stdout, stderr
    except subprocess.TimeoutExpired:
        # communicate's timeout leaves the direct child unreaped, so this PGID
        # still belongs to the fixture and cannot be recycled to another job.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        raise RuntimeError("Native process fixture timed out\n" + stdout.decode("utf-8", "replace") +
                           stderr.decode("utf-8", "replace"))
    finally:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        cleanup_owned_group(process.pid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("OPENHARMONY_SDK_PATH"), help="Native package, SDK directory, or oo SDK view")
    parser.add_argument("--cxx", default=os.environ.get("CXX"))
    parser.add_argument("--sign-tool", default=shutil.which("binary-sign-tool"))
    parser.add_argument("--timeout", type=float, default=90, help="Fixture timeout, including owned descendant cleanup")
    parser.add_argument("--output", type=Path, help="Optional fresh evidence directory; no fixture binaries are retained")
    args = parser.parse_args()
    if sys.platform != "ohos":
        parser.error("Run this regression on the native OpenHarmony host")
    if not args.sdk or not args.sign_tool:
        parser.error("Provide --sdk and install binary-sign-tool in PATH (or pass --sign-tool)")
    try:
        sdk = native_sdk(args.sdk)
    except ValueError as error:
        parser.error(str(error))
    tmpdir = os.environ.get("TMPDIR")
    if not tmpdir or not Path(tmpdir).is_dir() or args.timeout <= 0:
        parser.error("TMPDIR must be an existing writable directory and timeout must be positive")
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    tests = Path(__file__).resolve().parent
    source = tests / "native_process_fixture/process.cpp"
    header = tests.parent / "process_openharmony.h"
    cxx = args.cxx or str(sdk / "llvm/bin/clang++")
    restore = enable_subreaper()
    try:
        with tempfile.TemporaryDirectory(prefix="godot-process-execution-", dir=tmpdir) as temporary:
            fixture = Path(temporary) / "native process fixture"
            fixture.mkdir()
            unsigned = fixture / "process.unsigned"
            executable = fixture / "process 'single' \"double\""
            compile_command = [cxx, "--target=aarch64-linux-ohos", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                               "--sysroot=" + str(sdk / "sysroot"), "-I" + str(tests.parent),
                               str(source), "-o", str(unsigned)]
            signer_command = [args.sign_tool, "sign", "-selfSign", "1", "-inFile", str(unsigned), "-outFile", str(executable)]
            commands = []
            for command in (compile_command, signer_command):
                completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
                commands.append({"command": command, "exitCode": completed.returncode,
                                 "output": completed.stdout.decode("utf-8", "replace")})
                if completed.returncode:
                    if args.output:
                        (args.output / "build.json").write_text(json.dumps(commands, indent=2) + "\n")
                    raise RuntimeError("Compiler/signing command failed:\n" + commands[-1]["output"])
            executable.chmod(0o700)  # TMPDIR is a real permission-preserving filesystem.
            environment = dict(os.environ)
            environment["GODOT_PROCESS_TEST_SUBREAPER"] = "1" if restore else "0"
            environment.pop("LD_PRELOAD", None)
            environment.pop("LD_LIBRARY_PATH", None)
            code, stdout, stderr = run_fixture([str(executable), str(fixture)], environment, args.timeout)
            text = stdout.decode("utf-8", "replace")
            errors = stderr.decode("utf-8", "replace")
            print(text, end="")
            if errors:
                print(errors, end="", file=sys.stderr)
            report = {"platform": sys.platform, "nativeSdk": str(sdk), "exitCode": code,
                      "helperSha256": hashlib.sha256(header.read_bytes()).hexdigest(),
                      "fixtureSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                      "signedExecutableSha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                      "subreaperCleanupEnabled": restore is not None,
                      "passed": [line.removeprefix("PASS: ") for line in text.splitlines() if line.startswith("PASS: ")],
                      "skipped": [line.removeprefix("SKIP: ") for line in text.splitlines() if line.startswith("SKIP: ")],
                      "scope": "Actual std/POSIX helper, native terminal. Not Godot UIAbility/HAP execution or a 64 MiB cap stress test."}
            if args.output:
                (args.output / "build.json").write_text(json.dumps(commands, indent=2) + "\n")
                (args.output / "fixture-stdout.log").write_text(text)
                (args.output / "fixture-stderr.log").write_text(errors)
                (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
            if code or "OHOS_PROCESS_EXECUTION_PASS" not in text:
                raise RuntimeError("Native process fixture failed (exit " + str(code) + ")")
    finally:
        if restore:
            restore()


if __name__ == "__main__":
    main()
