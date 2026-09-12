#!/usr/bin/env python3
"""Link/sign a native headless CLI and sign its Godot shared library for validation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library', type=Path, required=True)
parser.add_argument('--native-sdk', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True, help='New isolated directory')
args = parser.parse_args()
if sys.platform != 'ohos': raise SystemExit('Run on native OpenHarmony')
compiler, signer = shutil.which('clang++'), shutil.which('binary-sign-tool')
if not compiler or not signer: raise SystemExit('Check native LLVM and binary-sign-tool in PATH')
source = Path(__file__).resolve().parents[2]
args.output.mkdir(parents=True, exist_ok=False)
output = args.output.resolve()
with (output / 'build.log').open('w') as log:
    commands = [
        [signer, 'sign', '-inFile', str(args.library.resolve()), '-outFile', str(output / 'libgodot.so'), '-selfSign', '1'],
        [compiler, '--target=aarch64-linux-ohos', '--sysroot=' + str(args.native_sdk.resolve() / 'sysroot'),
         str(source / 'platform/openharmony/tools/cli.cpp'), '-L' + str(output), '-lgodot',
         '-Wl,-rpath,$ORIGIN', '-o', str(output / 'godot.unsigned')],
        [signer, 'sign', '-inFile', str(output / 'godot.unsigned'), '-outFile', str(output / 'godot'), '-selfSign', '1'],
    ]
    for command in commands:
        log.write('+ ' + ' '.join(command) + '\n'); log.flush()
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
(output / 'godot').chmod(0o755)
version = subprocess.run([str(output / 'godot'), '--version'], check=True, capture_output=True, text=True).stdout.strip()
git = subprocess.run(['git', '-c', 'safe.directory=' + str(source), 'rev-parse', 'HEAD'],
                     cwd=source, capture_output=True, text=True)
revision = git.stdout.strip() if git.returncode == 0 else None
if revision and revision[:9] not in version:
    raise SystemExit('The engine version does not match this source commit; rebuild the engine before packaging')
metadata = {'platform': sys.platform, 'engine_version': version, 'source_commit': revision}
if revision:
    status = subprocess.run(['git', '-c', 'safe.directory=' + str(source), 'status', '--porcelain'],
                            cwd=source, capture_output=True, text=True, check=True)
    metadata['source_tree_clean'] = not status.stdout.strip()
for name in ('godot', 'libgodot.so'):
    with (output / name).open('rb') as stream:
        metadata[name + '_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
(output / 'build-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(output / 'godot')
