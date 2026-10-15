#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run byte-exact production mouse capture methods on native aarch64 HarmonyOS.

Requires an explicit SDK26 native package and binary-sign-tool in PATH. The
fixture uses the production HostFocusState; OS time and cursor API calls are
mocked. It never monitors input, changes a real cursor, or launches an app.
Prints one JSON receipt with original production method hashes and native case
results. --output optionally preserves that receipt in a new JSON file.
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


METHODS = (
    b'void DisplayServerOpenHarmony::update_mouse_focus(',
    b'void DisplayServerOpenHarmony::_mouse_update_mode(',
    b'void DisplayServerOpenHarmony::mouse_set_mode(',
    b'void DisplayServerOpenHarmony::mouse_set_mode_override(',
    b'void DisplayServerOpenHarmony::mouse_set_mode_override_enabled(',
    b'DisplayServerOpenHarmony::~DisplayServerOpenHarmony(',
)
HOST_QUERIES = (
    b'bool godot_host_is_window_active(',
    b'bool godot_host_is_input_focused(',
    b'bool godot_host_is_window_active_for_cleanup(',
    b'uint64_t godot_host_get_window_focus_generation(',
)
# Ignore braces in comments, ordinary strings, character literals and raw strings.
TOKENS = re.compile(
    rb'//[^\n]*|/\*.*?\*/|R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"|'
    rb'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.DOTALL)


def extract_braced(source, signature):
    """Return the original bytes from a unique signature through its closing brace."""
    if source.count(signature) != 1:
        raise RuntimeError(f'Expected one production definition: {signature!r}')
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
                return source[start:match.end()]
            if depth < 0:
                break
    raise RuntimeError(f'Unbalanced production definition: {signature!r}')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run_command(command, stage, timeout):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        detail = (result.stdout + result.stderr)[-6000:]
        raise RuntimeError(f'{stage} failed ({result.returncode}): {detail}')
    return result


def run_fixture(native_sdk, receipt):
    if sys.platform != 'ohos' or os.uname().machine != 'aarch64':
        raise RuntimeError('Run on native aarch64 HarmonyOS')
    sdk = native_sdk.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    metadata = json.loads((sdk / 'oh-uni-package.json').read_bytes())
    if not isinstance(metadata, dict) or metadata.get('apiVersion') not in (26, '26', '26.0.0') or metadata.get('path') != 'native':
        raise RuntimeError('--native-sdk must select an SDK26 native package')
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not (sdk / 'sysroot').is_dir() or not signer:
        raise RuntimeError('SDK26 llvm/bin/clang++, sysroot and PATH binary-sign-tool are required')
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    tests = Path(__file__).resolve().parent
    platform = tests.parent
    paths = {
        'production': platform / 'display_server_openharmony.cpp',
        'header': platform / 'display_server_openharmony.h',
        'host_focus_state': platform / 'host_focus_state.h',
        'host': platform / 'engine_host_openharmony.cpp',
        'mouse_enum': platform.parents[1] / 'servers/display/display_server_enums.h',
        'fixture': tests / 'native_mouse_capture.cpp',
        'runner': Path(__file__).resolve(),
    }
    snapshots = {name: path.read_bytes() for name, path in paths.items()}
    source = snapshots['production']
    extracted = [extract_braced(source, signature) for signature in METHODS]
    host_methods = b'\n\n'.join(extract_braced(snapshots['host'], signature) for signature in HOST_QUERIES) + b'\n'
    # Delimiters are added only between definitions; each definition stays exact.
    methods = b'\n\n'.join(extracted) + b'\n'
    member_match = re.search(
        rb'(?m)^\tDisplayServerEnums::MouseMode mouse_mode = [^\n]+;.*?'
        rb'^\tint mouse_mode_last_error = [^\n]+;', snapshots['header'], re.DOTALL)
    if not member_match:
        raise RuntimeError('Cannot locate production mouse capture member declarations')
    members = member_match.group()
    mouse_enum = extract_braced(snapshots['mouse_enum'], b'enum MouseMode {')
    enum_end = snapshots['mouse_enum'].index(b'enum MouseMode {') + len(mouse_enum)
    if snapshots['mouse_enum'][enum_end:enum_end + 1] != b';':
        raise RuntimeError('Production MouseMode enum must end with a semicolon')
    mouse_enum += b';'
    receipt.update({
        'target': 'aarch64-linux-ohos',
        'compiler': str(compiler),
        'native_sdk': str(sdk),
        'sdk_api': metadata['apiVersion'],
        'scope': 'production methods, members, enum and HostFocusState; mocked OS clock/cursor SDK; no real input or cursor calls',
        'source_sha256': {name: digest(data) for name, data in snapshots.items()},
        'original_method_sha256': {
            signature.decode().split('::', 1)[1].rstrip('('): digest(body)
            for signature, body in zip(METHODS, extracted)
        },
        'compiled_methods_sha256': digest(methods),
        'compiled_host_queries_sha256': digest(host_methods),
        'compiled_members_sha256': digest(members),
        'compiled_enum_sha256': digest(mouse_enum),
    })
    with tempfile.TemporaryDirectory(prefix='godot-mouse-capture-', dir=temporary_root) as temporary:
        directory = Path(temporary).resolve(strict=True)
        if directory.parent != temporary_root or not directory.name.startswith('godot-mouse-capture-'):
            raise RuntimeError('Unexpected native fixture directory')
        try:
            (directory / 'mouse_capture_methods.inc').write_bytes(methods)
            (directory / 'mouse_capture_host_queries.inc').write_bytes(host_methods)
            (directory / 'mouse_capture_members.inc').write_bytes(members + b'\n')
            (directory / 'mouse_capture_enum.inc').write_bytes(mouse_enum + b'\n')
            unsigned = directory / 'mouse-capture.unsigned'
            executable = directory / 'mouse-capture'
            # Use the selected SDK's compiler, never a PATH compiler that auto-signs.
            run_command([
                str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(directory), '-I' + str(platform),
                str(paths['fixture']), '-o', str(unsigned),
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
            # Check the exact isolated absolute directory before automatic cleanup.
            if directory.resolve(strict=True) != directory or directory.parent != temporary_root:
                raise RuntimeError('Native fixture cleanup path changed')
    receipt['temporary_fixture_removed'] = not directory.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True, help='SDK26 native package containing llvm/bin/clang++')
    parser.add_argument('--output', type=Path, help='Also write the JSON receipt to this new file; parent must exist')
    args = parser.parse_args()
    receipt = {'test': 'mouse_capture', 'passed': False}
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
