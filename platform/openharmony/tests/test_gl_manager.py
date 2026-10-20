#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Build, sign and run the production GLManagerOpenHarmony on native HarmonyOS.

The mock cases inject EGL failure/cleanup and thread migration boundaries. The
real probe uses the actual EGL/GLES driver with a pbuffer replacing only the
window surface/config surface bit. This does not validate an XComponent, HAP,
renderer integration or an Editor/template build. All build files live in TMPDIR
and are removed. --output can preserve a new JSON receipt outside that directory.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import types


LOG_STUB = r'''#pragma once
#include <cstdio>
#define ERR_PRINT(message) std::fprintf(stderr, "%s\n", message)
#define WARN_PRINT(message) ERR_PRINT(message)
#define ERR_FAIL_NULL_V(pointer, result) do { if (!(pointer)) { return result; } } while (0)
#define ERR_FAIL_COND_V_MSG(condition, result, message) do { if (condition) { ERR_PRINT(message); return result; } } while (0)
#define ERR_FAIL_COND_MSG(condition, message) do { if (condition) { ERR_PRINT(message); return; } } while (0)
'''


class BuildEnvironment(dict):
    def __init__(self, **values):
        super().__init__(values)
        self.drivers_sources = []

    def Append(self, **values):
        for key, value in values.items():
            self.setdefault(key, []).extend(value)

    def Prepend(self, **values):
        for key, value in values.items():
            self[key] = value + self.get(key, [])

    def Clone(self):
        return self

    def disable_warnings(self):
        pass

    def add_source_files(self, destination, sources):
        destination.extend(sources if isinstance(sources, list) else [sources])


def build_configuration_cases(root, sdk):
    """Execute production configure and GLAD SConscript without an engine build."""
    messages = []
    methods = types.ModuleType('methods')
    methods.print_error = messages.append
    platform_methods = types.ModuleType('platform_methods')
    platform_methods.validate_arch = lambda arch, name, supported: None
    saved = {name: sys.modules.get(name) for name in ('methods', 'platform_methods')}
    sys.modules.update(methods=methods, platform_methods=platform_methods)
    try:
        spec = importlib.util.spec_from_file_location('openharmony_gl_detect', root / 'platform/openharmony/detect.py')
        detect = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(detect)
        assert detect.get_flags()['vulkan'] is True and detect.get_flags()['opengl3'] is True
        count = 1
        for target in ('editor', 'template_debug', 'template_release'):
            for opengl3 in (True, False):
                env = BuildEnvironment(arch='arm64', target=target, generate_bundle=False,
                                       OPENHARMONY_SDK_PATH=str(sdk), vulkan=True, opengl3=opengl3, use_volk=False)
                detect.configure(env)
                assert ('GLES3_ENABLED' in env['CPPDEFINES']) == opengl3
                assert 'VULKAN_ENABLED' in env['CPPDEFINES'] and 'RD_ENABLED' in env['CPPDEFINES']
                assert 'vulkan' in env['LIBS'] and 'GLESv2' not in env['LIBS']
                count += 1
        for opengl3 in (True, False):
            env = BuildEnvironment(arch='arm64', target='template_release', generate_bundle=False,
                                   OPENHARMONY_SDK_PATH=str(sdk), vulkan=False, opengl3=opengl3, use_volk=True)
            try:
                detect.configure(env)
            except SystemExit as error:
                assert error.code == 255
            else:
                raise AssertionError('GL-only/no-backend must be rejected until Vulkan platform dependencies are guarded')
            count += 1
    finally:
        for name, original in saved.items():
            if original is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = original
    env = BuildEnvironment(platform='openharmony')
    sys.path.insert(0, str(root))
    try:
        runpy.run_path(str(root / 'drivers/gl_context/SCsub'), init_globals={'env': env, 'Import': lambda name: None})
    finally:
        sys.path.pop(0)
    assert '#thirdparty/glad/gl.c' in env.drivers_sources and '#thirdparty/glad/egl.c' in env.drivers_sources
    assert {'GLAD_ENABLED', 'EGL_ENABLED', 'GLAD_GLES2', 'EGL_NO_PLATFORM_SPECIFIC_TYPES'} <= set(env['CPPDEFINES'])
    return count + 1


def run(command, stage, timeout=60):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{stage} failed ({result.returncode}): {(result.stdout + result.stderr)[-8000:]}')
    return result


def run_fixture(sdk_path, receipt):
    if sys.platform != 'ohos' or os.uname().machine != 'aarch64':
        raise RuntimeError('Run on native aarch64 HarmonyOS')
    sdk = sdk_path.resolve(strict=True)
    metadata = json.loads((sdk / 'oh-uni-package.json').read_bytes())
    if str(metadata.get('apiVersion')) != '26' or metadata.get('path') != 'native':
        raise RuntimeError('--native-sdk must select an SDK26 native package')
    compiler = sdk / 'llvm/bin/clang++'
    c_compiler = sdk / 'llvm/bin/clang'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not c_compiler.is_file() or not signer:
        raise RuntimeError('SDK26 clang/clang++ and PATH binary-sign-tool are required')
    root = Path(__file__).resolve().parents[3]
    platform = root / 'platform/openharmony'
    snapshots = {
        name: (root / name).read_bytes() for name in (
            'platform/openharmony/gl_manager_openharmony.cpp', 'platform/openharmony/gl_manager_openharmony.h',
            'platform/openharmony/platform_gl.h', 'platform/openharmony/platform_egl.h',
            'platform/openharmony/detect.py', 'drivers/gl_context/SCsub',
            'platform/openharmony/tests/native_gl_manager.cpp', 'platform/openharmony/tests/test_gl_manager.py',
            'core/error/error_list.h', 'servers/display/display_server_enums.h',
            'thirdparty/glad/egl.c', 'thirdparty/glad/gl.c',
            'thirdparty/glad/glad/egl.h', 'thirdparty/glad/glad/gl.h', 'thirdparty/glad/EGL/eglplatform.h',
        )
    }
    digest = lambda data: hashlib.sha256(data).hexdigest()
    receipt.update(target='aarch64-linux-ohos', compiler=str(compiler), native_sdk=str(sdk), sdk_api=metadata['apiVersion'],
                   scope='Complete production manager, SDK native window types and GLAD; logging only stubbed. Mock EGL failures; real EGL/GLES pbuffer replaces window/config surface boundary. No XComponent/HAP/engine build.',
                   source_sha256={name: digest(data) for name, data in snapshots.items()})
    receipt['build_configuration_cases'] = build_configuration_cases(root, sdk)
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-gl-manager-', dir=temporary_root) as temporary:
        directory = Path(temporary).resolve(strict=True)
        if directory.parent != temporary_root or not directory.name.startswith('godot-gl-manager-'):
            raise RuntimeError('Unexpected native fixture directory')
        try:
            stub_directory = directory / 'core/error'
            stub_directory.mkdir(parents=True)
            (stub_directory / 'error_macros.h').write_text(LOG_STUB, encoding='utf-8')
            common = ['--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'), '-O2',
                      '-DGLAD_GLES2', '-DEGL_NO_PLATFORM_SPECIFIC_TYPES',
                      '-I' + str(root / 'thirdparty/glad')]
            objects = []
            for name in ('gl', 'egl'):
                obj = directory / (name + '.o')
                run([str(c_compiler), *common, '-std=c11', '-c', str(root / 'thirdparty/glad' / (name + '.c')), '-o', str(obj)], 'compile GLAD ' + name)
                objects.append(str(obj))
            unsigned = directory / 'gl-manager.unsigned'
            executable = directory / 'gl-manager'
            run([str(compiler), *common, '-std=c++17', '-DGLES3_ENABLED', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(directory), '-I' + str(root), '-I' + str(platform),
                 str(platform / 'gl_manager_openharmony.cpp'), str(platform / 'tests/native_gl_manager.cpp'), *objects,
                 '-Wl,--wrap=gladLoaderLoadEGL', '-Wl,--wrap=gladLoadGLES2', '-ldl', '-o', str(unsigned)], 'compile manager and fixture')
            # Some SDK linkers sign automatically. Remove that section before
            # the explicit final signing, without ever running the unsigned file.
            objcopy = sdk / 'llvm/bin/llvm-objcopy'
            run([str(objcopy), '--remove-section=.codesign', str(unsigned)], 'remove linker signature')
            run([signer, 'sign', '-inFile', str(unsigned), '-outFile', str(executable), '-selfSign', '1'], 'sign')
            executable.chmod(0o755)
            receipt['signed_executable_sha256'] = digest(executable.read_bytes())
            receipt['native'] = {}
            for mode in ('mock', 'real'):
                result = run([str(executable), mode], 'native ' + mode, 30)
                native = json.loads(result.stdout)
                if not native.get('passed'):
                    raise RuntimeError('Native fixture did not pass: ' + result.stdout)
                receipt['native'][mode] = native
                if mode == 'mock':
                    # Two failures per episode must emit only one diagnostic;
                    # the successful application between episodes resets it.
                    assert result.stderr.count('Cannot apply the EGL swap interval.') == 2
                    assert result.stderr.count('Cannot apply the EGL swap interval. EGL error: 0x300d.') == 2
                if result.stderr:
                    receipt['native'][mode]['stderr'] = result.stderr[-8000:]
            changed = [name for name, data in snapshots.items() if (root / name).read_bytes() != data]
            if changed:
                raise RuntimeError('Production/test sources changed during native run: ' + ', '.join(changed))
            receipt['passed'] = True
        finally:
            if directory.resolve(strict=True) != directory or directory.parent != temporary_root:
                raise RuntimeError('Native fixture cleanup path changed')
    receipt['temporary_fixture_removed'] = not directory.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, help='Also save this receipt to a new file; parent must exist')
    args = parser.parse_args()
    receipt = {'test': 'gl_manager', 'passed': False}
    try:
        run_fixture(args.native_sdk, receipt)
    except (OSError, ValueError, RuntimeError, KeyError, AssertionError, subprocess.TimeoutExpired) as error:
        receipt.update(passed=False, error=str(error))
    text = json.dumps(receipt, sort_keys=True, separators=(',', ':')) + '\n'
    if args.output:
        try:
            with args.output.open('x', encoding='utf-8') as output:
                output.write(text)
        except OSError as error:
            receipt.update(passed=False, error=f'Cannot create output receipt: {error}')
            text = json.dumps(receipt, sort_keys=True, separators=(',', ':')) + '\n'
    print(text, end='')
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
