#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run the current production cursor-notify method with SDK failures, without IME attach.

The complete method is extracted byte-for-byte from production C++; a small native
owner substitutes mapping/state/logging and Notify, while real SDK CursorInfo
creation/destruction is wrapped for ownership checks and allocation fault injection.
--baseline removes only the failure return to reproduce the old cache bug (exit 1).
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def extract_method(source):
    signature = b'void DisplayServerOpenHarmony::_update_ime_cursor() {'
    if source.count(signature) != 1:
        raise RuntimeError('Expected one complete production cursor update method')
    start = source.index(signature)
    # Ignore braces inside comments and strings; preserve every original byte.
    tokens = rb'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    depth = 0
    for token in re.finditer(tokens, source[start:], re.DOTALL):
        if token.group() == b'{':
            depth += 1
        elif token.group() == b'}':
            depth -= 1
            if depth == 0:
                return source[start:start + token.end()]
    raise RuntimeError('Unterminated production cursor update method')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true', help='Restore old unconditional cache behavior; expect exit 1')
    args = parser.parse_args()
    if sys.platform != 'ohos' or os.uname().machine != 'aarch64':
        raise SystemExit('Run on native aarch64 HarmonyOS')
    sdk = args.native_sdk.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not signer:
        raise SystemExit('Native SDK clang++ and binary-sign-tool are required')
    tests = Path(__file__).resolve().parent
    production = tests.parent / 'display_server_openharmony.cpp'
    source = production.read_bytes()
    method = extract_method(source)
    compiled_method = method
    if args.baseline:
        # A single mutation restores the pre-fix behavior; no mirrored method.
        compiled_method, count = re.subn(
            rb'(\tif \(code != IME_ERR_OK\) \{\n\t\tERR_PRINT\([^\n]+\);\n)\t\treturn;\n',
            rb'\1', method)
        if count != 1:
            raise RuntimeError('Cannot restore the old failure fallthrough from this method')
    mode = 'baseline' if args.baseline else 'fixed'
    digest = lambda data: hashlib.sha256(data).hexdigest()
    line = source[:source.index(method)].count(b'\n') + 1
    print(f'{mode}: production={production}:{line} source_sha256={digest(source)}', flush=True)
    print(f'extracted_method_sha256={digest(method)} compiled_method_sha256={digest(compiled_method)}', flush=True)
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    directory = Path(tempfile.mkdtemp(prefix='godot-ime-cursor-notify-', dir=temporary_root)).resolve(strict=True)
    if directory.parent != temporary_root or not directory.name.startswith('godot-ime-cursor-notify-'):
        raise RuntimeError('Unexpected native fixture directory')
    try:
        (directory / 'ime_cursor_method.inc').write_bytes(compiled_method)
        unsigned = directory / 'notify.unsigned'
        executable = directory / 'notify'
        compile_command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                           '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(directory),
                           str(tests / 'native_ime_cursor_notify.cpp'),
                           '-Wl,--wrap=OH_CursorInfo_Create', '-Wl,--wrap=OH_CursorInfo_Destroy',
                           '-Wl,--wrap=OH_InputMethodProxy_NotifyCursorUpdate', '-lohinputmethod', '-o', str(unsigned)]
        commands = [compile_command,
                    [signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)]]
        for label, command in zip(('compile', 'sign'), commands):
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            print(f'{mode} {label}_exit={result.returncode}', flush=True)
            if result.returncode:
                print(result.stdout + result.stderr, file=sys.stderr)
                return result.returncode
        executable.chmod(0o755)
        print(f'compiler={compiler} target=aarch64-linux-ohos optimization=-O2 SDK={sdk}', flush=True)
        print(f'{mode} signed_runner_sha256={digest(executable.read_bytes())}', flush=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        print(f'{mode} native_exit={result.returncode}', flush=True)
        return result.returncode
    finally:
        # Resolve and verify this exact isolated absolute path before deleting it.
        if directory.resolve(strict=True) != directory or directory.parent != temporary_root:
            raise RuntimeError('Native fixture cleanup path changed')
        shutil.rmtree(directory)
        print(f'{mode} temporary_fixture_removed={not directory.exists()}', flush=True)


if __name__ == '__main__':
    sys.exit(main())
