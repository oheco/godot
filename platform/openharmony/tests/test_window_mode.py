#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run the unchanged HostWindowModeState class on native aarch64 HarmonyOS.

Uses an explicitly selected SDK26 native compiler and PATH binary-sign-tool.
Only the focused pure-state fixture is compiled, signed and executed, in a
TemporaryDirectory under TMPDIR; there are no window SDK calls or app launches.
Prints a JSON receipt with source/class hashes and native cases. --output may
preserve the receipt in a new file. Run after host_window_mode_state.h exists.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


# Preserve source bytes while ignoring braces in comments and C++ literals.
TOKENS = re.compile(
    rb'//[^\n]*|/\*.*?\*/|R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"|'
    rb'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.DOTALL)


def extract_class(source):
    signature = b'class HostWindowModeState'
    if source.count(signature) != 1:
        raise RuntimeError('Expected one production HostWindowModeState definition')
    start = source.index(signature)
    depth = 0
    opened = False
    for match in TOKENS.finditer(source, start):
        token = match.group()
        if token == b'{':
            opened = True
            depth += 1
        elif token == b'}':
            depth -= 1
            if opened and depth == 0:
                tail = re.match(rb'\s*;', source[match.end():])
                if not tail:
                    raise RuntimeError('Production HostWindowModeState must end with a semicolon')
                return source[start:match.end() + tail.end()]
            if depth < 0:
                break
    raise RuntimeError('Unbalanced production HostWindowModeState definition')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run_command(command, stage, timeout):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{stage} failed ({result.returncode}): ' + (result.stdout + result.stderr)[-6000:])
    return result


def run_fixture(native_sdk, receipt):
    if sys.platform != 'ohos' or os.uname().machine != 'aarch64':
        raise RuntimeError('Run on native aarch64 HarmonyOS')
    sdk = native_sdk.resolve(strict=True)
    metadata = json.loads((sdk / 'oh-uni-package.json').read_bytes())
    if (not isinstance(metadata, dict) or metadata.get('apiVersion') not in (26, '26', '26.0.0')
            or metadata.get('path') != 'native'):
        raise RuntimeError('--native-sdk must select an SDK26 native package')
    compiler = sdk / 'llvm/bin/clang++'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not (sdk / 'sysroot').is_dir() or not signer:
        raise RuntimeError('SDK26 llvm/bin/clang++, sysroot and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    tests = Path(__file__).resolve().parent
    paths = {
        'production': tests.parent / 'host_window_mode_state.h',
        'fixture': tests / 'native_window_mode.cpp',
        'runner': Path(__file__).resolve(),
    }
    snapshots = {name: path.read_bytes() for name, path in paths.items()}
    production_class = extract_class(snapshots['production'])
    receipt.update({
        'target': 'aarch64-linux-ohos',
        'compiler': str(compiler),
        'signer': signer,
        'native_sdk': str(sdk),
        'sdk_api': metadata['apiVersion'],
        'scope': 'unchanged production HostWindowModeState public API; no real window or input SDK calls',
        'source_sha256': {name: digest(data) for name, data in snapshots.items()},
        'compiled_class_sha256': digest(production_class),
    })
    with tempfile.TemporaryDirectory(prefix='godot-window-mode-', dir=temporary_root) as temporary:
        directory = Path(temporary).resolve(strict=True)
        if directory.parent != temporary_root or not directory.name.startswith('godot-window-mode-'):
            raise RuntimeError('Unexpected native fixture directory')
        try:
            (directory / 'window_mode_state.inc').write_bytes(production_class)
            unsigned = directory / 'window-mode.unsigned'
            executable = directory / 'window-mode'
            # Select the real SDK compiler directly, avoiding PATH auto-sign wrappers.
            run_command([
                str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(directory), str(paths['fixture']), '-o', str(unsigned),
            ], 'compile', 60)
            run_command([
                signer, 'sign', '-inFile', str(unsigned), '-outFile', str(executable), '-selfSign', '1',
            ], 'sign', 60)
            executable.chmod(0o755)
            receipt['signed_executable_sha256'] = digest(executable.read_bytes())
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
            receipt['native_exit'] = result.returncode
            receipt['native'] = json.loads(result.stdout)
            if result.stderr:
                receipt['native_stderr'] = result.stderr[-6000:]
            changed = [name for name, path in paths.items() if path.read_bytes() != snapshots[name]]
            if changed:
                raise RuntimeError('Sources changed during the focused native run: ' + ', '.join(changed))
            receipt['passed'] = result.returncode == 0 and receipt['native'].get('passed') is True
        finally:
            # Validate this isolated absolute path before TemporaryDirectory deletes it.
            if directory.resolve(strict=True) != directory or directory.parent != temporary_root:
                raise RuntimeError('Native fixture cleanup path changed')
    receipt['temporary_fixture_removed'] = not directory.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True,
                        help='SDK26 native package containing llvm/bin/clang++')
    parser.add_argument('--output', type=Path, help='Also create a new JSON receipt; parent must exist')
    args = parser.parse_args()
    receipt = {'test': 'window_mode_native', 'passed': False}
    try:
        run_fixture(args.native_sdk, receipt)
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as error:
        receipt['passed'] = False
        receipt['error'] = str(error)
    text = json.dumps(receipt, sort_keys=True, separators=(',', ':')) + '\n'
    if args.output:
        try:
            with args.output.open('x', encoding='utf-8') as output:
                output.write(text)
        except OSError as error:
            receipt['passed'] = False
            receipt['error'] = f'Cannot create output receipt: {error}'
            text = json.dumps(receipt, sort_keys=True, separators=(',', ':')) + '\n'
    print(text, end='')
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
