#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Run current export rendering validation with real native Editor configuration.

Requires a native SDK, binary-sign-tool and an existing signed Editor library.
Only the small fixture and the byte-exact production validation method compile;
no SCons, engine rebuild, template extraction, GPU, HDC or installation occurs.
Generated headers can come from a previous build of this Godot version. Each
case boots a separate headless Editor with independent project/XDG state while
preserving the real HOME. Only the missing-texture-import case substitutes the
OS texture preference to model an x86 export host; the importer check stays real.
All generated files and a self-signed copy of the library live in TMPDIR and are
removed on completion. The supplied library is never modified.

--library-only tests the method in a rebuilt library instead of compiling it.
--expect-original-bug uses the old library method, requiring a successful Vulkan
control and the original Vulkan-only rejection of two legal GLES configurations.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SIGNATURE = b'bool EditorExportPlatformOpenHarmony::has_valid_project_configuration('
TOKENS = re.compile(rb'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.DOTALL)
METHOD = 'renderer/rendering_method'
RD = 'rendering_device/driver'
GL = 'gl_compatibility/driver'


def extract_method(source):
    """Keep the entire production definition byte-exact, including other checks."""
    if source.count(SIGNATURE) != 1:
        raise RuntimeError('Expected exactly one production project validation method')
    start = source.index(SIGNATURE)
    depth = 0
    opened = False
    for match in TOKENS.finditer(source, start):
        if match.group() == b'{':
            opened = True
            depth += 1
        elif match.group() == b'}':
            depth -= 1
            if opened and depth == 0:
                return source[start:match.end()]
    raise RuntimeError('Unbalanced production validation method')


def configuration(name, settings, method='gl_compatibility', driver='opengl3',
                  valid=True, error='', features='', texture=True, texture_preference='native'):
    return dict(name=name, settings=settings, method=method, driver=driver,
                valid=valid, error=error, features=features, texture=texture,
                texture_preference=texture_preference)


def cases():
    # Keep property order: the engine uses the first registered matching override.
    return [
        configuration('default_mobile_vulkan', [], method='mobile', driver='vulkan'),
        configuration('gles_mobile_driver', [(METHOD + '.mobile', 'gl_compatibility')]),
        configuration('gles_ignores_rd', [(METHOD + '.mobile', 'gl_compatibility'), (RD + '.openharmony', 'd3d12')]),
        configuration('forward_plus_ignores_gl', [(METHOD + '.mobile', 'forward_plus'), (GL + '.openharmony', 'opengl3_angle')], method='forward_plus', driver='vulkan'),
        configuration('mobile_ignores_gl', [(METHOD + '.mobile', 'mobile'), (GL + '.openharmony', 'metal')], method='mobile', driver='vulkan'),
        configuration('openharmony_before_mobile_gles', [(METHOD + '.openharmony', 'gl_compatibility'), (METHOD + '.mobile', 'mobile')]),
        configuration('mobile_before_openharmony_gles', [(METHOD + '.mobile', 'gl_compatibility'), (METHOD + '.openharmony', 'mobile')]),
        configuration('openharmony_before_mobile_vulkan', [(METHOD + '.openharmony', 'mobile'), (METHOD + '.mobile', 'gl_compatibility')], method='mobile', driver='vulkan'),
        configuration('mobile_before_openharmony_vulkan', [(METHOD + '.mobile', 'mobile'), (METHOD + '.openharmony', 'gl_compatibility')], method='mobile', driver='vulkan'),
        configuration('gles_ignores_editor_driver', [(METHOD + '.mobile', 'gl_compatibility'), (GL + '.editor', 'opengl3_angle'), (GL + '.openharmony', 'opengl3')]),
        configuration('gles_ignores_desktop_method', [(METHOD + '.desktop', 'forward_plus'), (METHOD + '.mobile', 'gl_compatibility'), (RD + '.openharmony', 'metal')]),
        configuration('gles_custom_feature', [(METHOD + '.gles_fixture', 'gl_compatibility'), (METHOD + '.mobile', 'mobile')], features='gles_fixture'),
        configuration('gles_inactive_custom_feature', [(METHOD + '.gles_fixture', 'unknown'), (METHOD + '.mobile', 'gl_compatibility')]),
        configuration('gles_platform_driver', [(METHOD + '.mobile', 'gl_compatibility'), (GL, 'opengl3_angle'), (GL + '.openharmony', 'opengl3')]),
        configuration('vulkan_platform_driver', [(METHOD + '.mobile', 'mobile'), (RD, 'metal'), (RD + '.openharmony', 'vulkan')], method='mobile', driver='vulkan'),
        configuration('gles_driver_case', [(METHOD + '.mobile', 'gl_compatibility'), (GL + '.openharmony', 'OPENGL3')], driver='OPENGL3'),
        configuration('vulkan_driver_case', [(METHOD + '.mobile', 'mobile'), (RD + '.openharmony', 'VULKAN')], method='mobile', driver='VULKAN'),
        configuration('gles_rejects_angle', [(METHOD + '.mobile', 'gl_compatibility'), (GL + '.openharmony', 'opengl3_angle')], driver='opengl3_angle', valid=False, error='requires the OpenGL 3 driver'),
        configuration('openharmony_gles_rejects_angle', [(METHOD + '.openharmony', 'gl_compatibility'), (METHOD + '.mobile', 'mobile'), (GL + '.openharmony', 'opengl3_angle')], driver='opengl3_angle', valid=False, error='requires the OpenGL 3 driver'),
        configuration('openharmony_mobile_rejects_metal', [(METHOD + '.openharmony', 'mobile'), (METHOD + '.mobile', 'gl_compatibility'), (RD + '.openharmony', 'metal')], method='mobile', driver='metal', valid=False, error='require the Vulkan rendering driver'),
        configuration('gles_rejects_vulkan_driver', [(METHOD + '.mobile', 'gl_compatibility'), (GL + '.openharmony', 'vulkan')], driver='vulkan', valid=False, error='requires the OpenGL 3 driver'),
        configuration('mobile_rejects_metal', [(METHOD + '.mobile', 'mobile'), (RD + '.openharmony', 'metal')], method='mobile', driver='metal', valid=False, error='require the Vulkan rendering driver'),
        configuration('forward_plus_rejects_gl_driver', [(METHOD + '.mobile', 'forward_plus'), (RD + '.openharmony', 'opengl3')], method='forward_plus', driver='opengl3', valid=False, error='require the Vulkan rendering driver'),
        configuration('rejects_unknown_method', [(METHOD + '.mobile', 'unknown')], method='unknown', driver='vulkan', valid=False, error='Compatibility, Mobile or Forward+ rendering method'),
        configuration('gles_native_texture_preference', [(METHOD + '.mobile', 'gl_compatibility')], texture=False),
        configuration('gles_still_requires_etc2_astc', [(METHOD + '.mobile', 'gl_compatibility')], valid=False, error='ETC2/ASTC texture compression must be enabled', texture=False, texture_preference='s3tc_bptc'),
    ]


def run_command(command, root, environment, timeout=90):
    result = subprocess.run(command, cwd=root, env=environment, text=True,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): ' +
                           (result.stdout + result.stderr)[-8000:])
    return result


def isolated_environment(root):
    environment = dict(os.environ)
    for variable in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME',
                     'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'GODOT_OHOS_DATA_DIR',
                     'GODOT_OHOS_CACHE_DIR', 'TMPDIR'):
        directory = root / variable.lower()
        directory.mkdir(mode=0o700)
        environment[variable] = str(directory)
    return environment


def run_fixture(args, receipt):
    if sys.platform != 'ohos' or os.uname().machine != 'aarch64':
        raise RuntimeError('Run on native aarch64 HarmonyOS')
    repo = Path(__file__).resolve().parents[3]
    sdk = args.sdk.resolve(strict=True)
    source_library = args.library.resolve(strict=True)
    compiler = sdk / 'llvm/bin/clang++'
    objcopy = sdk / 'llvm/bin/llvm-objcopy'
    signer = shutil.which('binary-sign-tool')
    if not compiler.is_file() or not objcopy.is_file() or not (sdk / 'sysroot').is_dir() or not signer:
        raise RuntimeError('Native SDK llvm/bin/clang++, llvm-objcopy, sysroot and PATH binary-sign-tool are required')
    source_path = repo / 'platform/openharmony/export/export_plugin.cpp'
    method = extract_method(source_path.read_bytes())
    library_only = args.library_only or args.expect_original_bug
    receipt.update(target='aarch64-linux-ohos', sdk=str(sdk), library=str(source_library),
                   mode='original-bug' if args.expect_original_bug else ('library-only' if library_only else 'current-production-method'),
                   production_method_sha256=hashlib.sha256(method).hexdigest())
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-rendering-export-', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        if root.parent != temporary_root or not root.name.startswith('godot-rendering-export-'):
            raise RuntimeError('Unexpected native fixture directory')
        try:
            environment = isolated_environment(root)
            (root / 'rendering_export_method.inc').write_bytes(method)
            library = root / 'libgodot.so'
            staging_library = root / 'libgodot.staging.so'
            run_command([str(objcopy), '--remove-section=.codesign', str(source_library), str(staging_library)], root, environment)
            run_command([signer, 'sign', '-selfSign', '1', '-inFile', str(staging_library), '-outFile', str(library)], root, environment)
            unsigned, executable = root / 'fixture.unsigned', root / 'fixture'
            command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
                       '-std=c++17', '-DOPENHARMONY_ENABLED', '-DUNIX_ENABLED', '-DDEBUG_ENABLED',
                       '-DDEBUG_METHODS_ENABLED', '-DTOOLS_ENABLED', '-DTHREADS_ENABLED',
                       '-D__OPEN_HARMONY__', '-DVULKAN_ENABLED', '-DRD_ENABLED', '-pthread',
                       '-I' + str(root), '-I' + str(repo), '-I' + str(repo / 'platform/openharmony')]
            if args.generated_headers:
                command.append('-I' + str(args.generated_headers.resolve(strict=True)))
            if library_only:
                command.append('-DGODOT_RENDERING_EXPORT_LIBRARY_ONLY')
            command += [str(repo / 'platform/openharmony/tests/native_rendering_export.cpp'),
                        str(repo / 'platform/openharmony/bridge_openharmony.cpp'), str(library),
                        '-Wl,--no-code-sign', '-Wl,-rpath,' + str(root), '-o', str(unsigned)]
            run_command(command, root, environment, timeout=120)
            run_command([str(objcopy), '--remove-section=.codesign', str(unsigned)], root, environment)
            run_command([signer, 'sign', '-selfSign', '1', '-inFile', str(unsigned), '-outFile', str(executable)], root, environment)
            executable.chmod(0o755)
            selected = cases()
            if args.expect_original_bug:
                selected = [case for case in selected if case['name'] in
                            ('default_mobile_vulkan', 'gles_mobile_driver', 'gles_ignores_rd')]
            if args.case:
                selected = [case for case in selected if case['name'] in args.case]
            if not selected:
                raise RuntimeError('No cases selected for this validation mode')
            def run_case(case):
                case_root = root / case['name']
                case_root.mkdir()
                case_environment = isolated_environment(case_root)
                valid = case['valid']
                error = case['error']
                if args.expect_original_bug and case['method'] == 'gl_compatibility':
                    valid, error = False, 'OpenHarmony export requires Vulkan renderer'
                case_environment.update({
                    'GODOT_RENDERING_CUSTOM_FEATURES': case['features'],
                    'GODOT_RENDERING_EXPECT_METHOD': case['method'],
                    'GODOT_RENDERING_EXPECT_DRIVER': case['driver'],
                    'GODOT_RENDERING_EXPECT_VALID': '1' if valid else '0',
                    'GODOT_RENDERING_EXPECT_ERROR': error,
                    'GODOT_RENDERING_TEXTURE_PREFERENCE': case['texture_preference'],
                })
                project = case_root / 'project'
                project.mkdir()
                project_file = project / 'project.godot'
                settings = case['settings'] + [('textures/vram_compression/import_etc2_astc', case['texture'])]
                project_bytes = ('config_version=5\n[application]\nconfig/name="Rendering export fixture"\n'
                                 '[rendering]\n' + '\n'.join(key + '=' + json.dumps(value) for key, value in settings) + '\n').encode()
                project_file.write_bytes(project_bytes)
                result = subprocess.run([str(executable), '--editor', '--headless', '--path', str(project)],
                                        cwd=case_root, env=case_environment, text=True, capture_output=True, timeout=90)
                passed = result.returncode == 0 and 'RENDERING_EXPORT_PASS failures=0' in result.stdout and project_file.read_bytes() == project_bytes
                record = dict(name=case['name'], passed=passed, native_exit=result.returncode)
                print(('PASS ' if passed else 'FAIL ') + case['name'], file=sys.stderr)
                if not passed:
                    record['output'] = (result.stdout + result.stderr)[-10000:]
                return record

            # Each child has its own project, settings, data and working directory.
            with ThreadPoolExecutor(max_workers=args.jobs) as executor:
                receipt['cases'] = list(executor.map(run_case, selected))
            if extract_method(source_path.read_bytes()) != method:
                raise RuntimeError('Production method changed during native verification')
            receipt['passed'] = bool(receipt['cases']) and all(case['passed'] for case in receipt['cases'])
        finally:
            # TemporaryDirectory removes only this exact previously checked root.
            if root.resolve(strict=True) != root or root.parent != temporary_root:
                raise RuntimeError('Unexpected native fixture cleanup path')
    receipt['temporary_fixture_removed'] = not root.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True, help='Native SDK package with llvm/bin/clang++ and sysroot')
    parser.add_argument('--library', type=Path, required=True, help='Existing signed Editor libgodot.so')
    parser.add_argument('--generated-headers', type=Path, help='Optional previous build source root supplying only missing generated headers')
    parser.add_argument('--case', action='append', choices=[case['name'] for case in cases()], help='Run only this named case; repeat to select several')
    parser.add_argument('--jobs', type=int, choices=(1, 2, 3, 4), default=1, help='Independent headless Editor processes (default: 1)')
    parser.add_argument('--library-only', action='store_true', help='Validate the linked Editor implementation without compiling the current method')
    parser.add_argument('--expect-original-bug', action='store_true', help='Require old library Vulkan control and original rejection of valid GLES configurations')
    args = parser.parse_args()
    receipt = dict(test='rendering_export', passed=False)
    try:
        run_fixture(args, receipt)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        receipt.update(passed=False, error=str(error))
    print(json.dumps(receipt, sort_keys=True))
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
