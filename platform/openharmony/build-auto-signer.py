#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Build/sign the standalone OpenHarmony C++ automatic-signing worker offline."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--deps-only', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Build and sign on native OpenHarmony')
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be between 1 and 32')
    repo = Path(__file__).resolve().parents[2]
    sdk = args.native_sdk.resolve(strict=True)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    signer = shutil.which('binary-sign-tool')
    if not signer:
        parser.error('PATH binary-sign-tool is required')
    llvm = sdk / 'llvm/bin'
    for name in ('clang', 'clang++', 'llvm-objcopy', 'llvm-readelf', 'llvm-ar'):
        if not (llvm/name).is_file():
            parser.error('Native SDK is missing '+name)
    jsoncpp = repo/'thirdparty/openxr/src/external/jsoncpp'
    mbed = repo/'thirdparty/mbedtls'
    source = repo/'platform/openharmony/export/auto_signer/auto_signer.cpp'
    common = ['--target=aarch64-linux-ohos', '--sysroot='+str(sdk/'sysroot'), '-O2', '-fPIC', '-pthread', '-ffunction-sections', '-fdata-sections']
    includes = ['-I'+str(jsoncpp/'include'), '-I'+str(mbed/'include'), '-I'+str(mbed/'library')]
    dependencies = [(path, False) for path in sorted((mbed/'library').glob('*.c')) if not path.name.startswith('godot_')]
    dependencies += [(path, True) for path in sorted((jsoncpp/'src/lib_json').glob('*.cpp'))]
    if not dependencies:
        raise RuntimeError('Vendored JsonCpp/mbedTLS sources are missing')
    object_dir = out/'objects'
    object_dir.mkdir(exist_ok=True)
    environment = dict(os.environ)
    environment.pop('LD_PRELOAD', None)
    records = []

    def run(command):
        result = subprocess.run([str(arg) for arg in command], env=environment, capture_output=True, timeout=180)
        if result.returncode:
            raise RuntimeError('Native worker build failed: '+result.stderr.decode('utf-8','replace')[:6000])
        return result

    def compile_one(item):
        path, cxx = item
        name = ('jsoncpp-' if cxx else 'mbedtls-')+path.stem
        obj = object_dir/(name+'.o')
        command = [llvm/('clang++' if cxx else 'clang'), *common, *includes, '-std='+('c++17' if cxx else 'c11'), '-c', path, '-o', obj]
        # Include all vendored headers in the fingerprint so configuration or
        # dependency upgrades cannot silently reuse stale objects.
        key = hashlib.sha256((str(path)+digest(path)+json.dumps([str(arg) for arg in command])+header_digest).encode()).hexdigest()
        stamp = obj.with_suffix('.sha256')
        cached = obj.is_file() and stamp.is_file() and stamp.read_text()==key
        if not cached:
            run(command)
            stamp.write_text(key)
        return {'source':str(path.relative_to(repo)), 'sha256':digest(path), 'object':str(obj), 'cached':cached}

    headers = sorted((mbed/'include').rglob('*.h'))+sorted((mbed/'library').glob('*.h'))+sorted((jsoncpp/'include').rglob('*.h'))
    header_digest = hashlib.sha256(''.join(str(path.relative_to(repo))+digest(path) for path in headers).encode()).hexdigest()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        records = list(pool.map(compile_one, dependencies))
    result = {'platform':sys.platform, 'nativeSdk':str(sdk), 'dependencyHeadersSha256':header_digest, 'dependencies':records, 'depsOnly':args.deps_only}
    if not args.deps_only:
        if not source.is_file():
            raise RuntimeError('Native C++ worker source is missing')
        worker_obj = object_dir/'auto_signer.o'
        run([llvm/'clang++', *common, *includes, '-std=c++17', '-fexceptions', '-frtti', '-c', source, '-o', worker_obj])
        unsigned, binary = out/'auto-sign.unsigned', out/'auto-sign'
        run([llvm/'clang++', *common, '-static-libstdc++', '-static-libgcc', '-Wl,--no-code-sign', '-Wl,--gc-sections', worker_obj, *[record['object'] for record in records], '-o', unsigned])
        dynamic = run([llvm/'llvm-readelf', '--dynamic', unsigned]).stdout.decode('utf-8','replace')
        if 'libc++_shared.so' in dynamic:
            raise RuntimeError('Worker must not depend on an App-provided libc++_shared.so')
        run([llvm/'llvm-objcopy', '--remove-section=.codesign', unsigned])
        run([signer, 'sign', '-selfSign', '1', '-inFile', unsigned, '-outFile', binary])
        binary.chmod(0o700)
        result.update(sourceSha256=digest(source), binary=str(binary), binarySha256=digest(binary), staticCppRuntime=True)
    (out/'build-info.json').write_text(json.dumps(result, indent=2)+'\n')
    print('NATIVE_AUTO_SIGNER_DEPENDENCIES_READY' if args.deps_only else 'NATIVE_AUTO_SIGNER_SIGNED', flush=True)


if __name__ == '__main__':
    main()
