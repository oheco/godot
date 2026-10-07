#!/usr/bin/env python3
"""Run real native IME object and wrapper geometry checks without attaching IME."""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'ohos':
        raise SystemExit('Run on native OpenHarmony')
    compiler = shutil.which('clang++')
    signer = shutil.which('binary-sign-tool')
    if not compiler or not signer:
        raise SystemExit('Native clang++ and binary-sign-tool must be in PATH')
    root = Path(os.environ['TMPDIR']).resolve(strict=True)
    platform = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='godot-ime-geometry-', dir=root) as temporary:
        directory = Path(temporary).resolve(strict=True)
        if directory.parent != root or not directory.name.startswith('godot-ime-geometry-'):
            raise RuntimeError('Unexpected native fixture directory')
        try:
            unsigned = directory / 'ime-geometry.unsigned'
            executable = directory / 'ime-geometry'
            commands = [
                [compiler, '--target=aarch64-linux-ohos',
                 '--sysroot=' + str(args.native_sdk.resolve() / 'sysroot'),
                 '-std=c++17', '-pthread', '-ffunction-sections', '-fdata-sections',
                 '-I' + str(platform), str(platform / 'tests/native_ime_geometry.cpp'),
                 str(platform / 'wrapper_openharmony.cpp'), '-Wl,--gc-sections',
                 '-lohinputmethod', '-o', str(unsigned)],
                [signer, 'sign', '-inFile', str(unsigned), '-outFile', str(executable), '-selfSign', '1'],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(f'Command failed ({result.returncode}): {result.stdout}{result.stderr}')
            executable.chmod(0o755)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            print(result.stdout, end='')
            if result.returncode:
                raise RuntimeError(f'Native fixture failed ({result.returncode}): {result.stderr}')
        finally:
            # Verify the exact isolated absolute path before TemporaryDirectory cleanup.
            if Path(temporary).resolve(strict=True) != directory or directory.parent != root:
                raise RuntimeError('Native fixture cleanup path changed')


if __name__ == '__main__':
    main()
