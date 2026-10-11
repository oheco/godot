#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run byte-exact production IME lifecycle methods on native aarch64 HarmonyOS.

Uses the SDK compiler, real TextEditorProxy/AttachOptions/TextConfig objects and
callback signatures. Only service-facing Attach/Detach/Show/Hide are substituted;
allocation wrappers inject failures and check ownership. Covers Detach-only focus/
visibility cleanup, exact numeric diagnostics, and display binding replacement.
Service results and geometry metadata are injected; no real IME service IPC,
engine rebuild, HAP, or actual keyboard-panel/display interaction is validated.
--baseline compiles these methods from git HEAD to demonstrate the regression.
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


METHODS = (
    b'void DisplayServerOpenHarmony::_get_text_config(',
    b'void DisplayServerOpenHarmony::_send_keyboard_status(',
    b'void DisplayServerOpenHarmony::virtual_keyboard_show(',
    b'void DisplayServerOpenHarmony::virtual_keyboard_hide(',
    b'void DisplayServerOpenHarmony::update_ime_focus(',
    b'void DisplayServerOpenHarmony::_update_ime_display(',
    b'int DisplayServerOpenHarmony::virtual_keyboard_get_height(',
)


def extract_method(source, signature):
    if source.count(signature) != 1:
        raise RuntimeError(f'Expected one production method: {signature!r}')
    start = source.index(signature)
    tokens = rb'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    depth = 0
    for token in re.finditer(tokens, source[start:], re.DOTALL):
        if token.group() == b'{':
            depth += 1
        elif token.group() == b'}':
            depth -= 1
            if depth == 0:
                return source[start:start + token.end()]
    raise RuntimeError('Unterminated production lifecycle method')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true', help='Compile git HEAD lifecycle methods; expect exit 1')
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
    header = tests.parent / 'display_server_openharmony.h'
    source = production.read_bytes()
    compiled_source = source
    if args.baseline:
        compiled_source = subprocess.run(
            ['git', 'show', 'HEAD:platform/openharmony/display_server_openharmony.cpp'],
            cwd=tests, check=True, capture_output=True, timeout=10).stdout
    if not args.baseline and b'bool DisplayServerOpenHarmony::_get_ime_screen_position(Point2 &r_position, uint32_t *r_display_id) const' not in compiled_source:
        raise RuntimeError('Wait for the production two-argument IME geometry helper before running')
    baseline_skips = {b'void DisplayServerOpenHarmony::update_ime_focus(',
                      b'void DisplayServerOpenHarmony::_update_ime_display('}
    methods = b'\n\n'.join(extract_method(compiled_source, method) for method in METHODS
                           if not (args.baseline and method in baseline_skips)) + b'\n'
    # Compile the current production member declarations too; the owner supplies
    # only the Godot types and unrelated callbacks needed by these methods.
    header_bytes = header.read_bytes()
    fixture_bytes = (tests / 'native_ime_client_lifecycle.cpp').read_bytes()
    members = re.search(rb'\tbool ime_active = false;.*?\tInputMethod_InputMethodProxy \*input_method_proxy = nullptr;',
                        header_bytes, re.DOTALL)
    if not members:
        raise RuntimeError('Cannot locate production IME member declarations')
    digest = lambda data: hashlib.sha256(data).hexdigest()
    mode = 'baseline' if args.baseline else 'fixed'
    print(f'{mode}: production={production} source_sha256={digest(source)}', flush=True)
    print(f'compiled_methods_sha256={digest(methods)} members_sha256={digest(members.group())}', flush=True)
    print(f'header_sha256={digest(header_bytes)} fixture_sha256={digest(fixture_bytes)}', flush=True)
    print('scope=real SDK editor/options/config objects and callbacks; Attach/Detach/Show/Hide service calls and display metadata mocked; no service IPC or HAP', flush=True)
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    directory = Path(tempfile.mkdtemp(prefix='godot-ime-client-lifecycle-', dir=temporary_root)).resolve(strict=True)
    if directory.parent != temporary_root or not directory.name.startswith('godot-ime-client-lifecycle-'):
        raise RuntimeError('Unexpected native fixture directory')
    try:
        (directory / 'ime_lifecycle_methods.inc').write_bytes(methods)
        (directory / 'ime_lifecycle_members.inc').write_bytes(members.group() + b'\n')
        unsigned = directory / 'lifecycle.unsigned'
        executable = directory / 'lifecycle'
        wrapped = ('OH_TextEditorProxy_Create', 'OH_TextEditorProxy_Destroy',
                   'OH_AttachOptions_Create', 'OH_AttachOptions_Destroy',
                   'OH_InputMethodController_Attach', 'OH_InputMethodController_Detach',
                   'OH_InputMethodProxy_ShowKeyboard', 'OH_InputMethodProxy_HideKeyboard')
        compile_command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                           '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                           '-pthread', '-I' + str(directory), str(tests / 'native_ime_client_lifecycle.cpp'),
                           *('-Wl,--wrap=' + symbol for symbol in wrapped), '-lohinputmethod', '-o', str(unsigned)]
        if args.baseline:
            compile_command.insert(1, '-DIME_LIFECYCLE_BASELINE=1')
        commands = (compile_command,
                    [signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)])
        for label, command in zip(('compile', 'sign'), commands):
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            print(f'{mode} {label}_exit={result.returncode}', flush=True)
            if result.returncode:
                print(result.stdout + result.stderr, file=sys.stderr)
                return result.returncode
        executable.chmod(0o755)
        print(f'compiler={compiler} target=aarch64-linux-ohos optimization=-O2 SDK={sdk}', flush=True)
        print(f'{mode} signed_runner_sha256={digest(executable.read_bytes())}', flush=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        print(f'{mode} native_exit={result.returncode}', flush=True)
        if production.read_bytes() != source or header.read_bytes() != header_bytes or (tests / 'native_ime_client_lifecycle.cpp').read_bytes() != fixture_bytes:
            raise RuntimeError('IME production/fixture changed during the narrow native run; receipt hashes identify the captured snapshot only')
        return result.returncode
    finally:
        if directory.resolve(strict=True) != directory or directory.parent != temporary_root:
            raise RuntimeError('Native fixture cleanup path changed')
        shutil.rmtree(directory)
        print(f'{mode} temporary_fixture_removed={not directory.exists()}', flush=True)


if __name__ == '__main__':
    sys.exit(main())
