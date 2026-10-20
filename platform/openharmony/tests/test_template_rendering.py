#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Check real bundle metadata generation and actual native Editor export gates.

--generation-only executes production SCsub.generate_bundle with SCons build I/O
adapters and explicitly non-executable ELF data. It stages real template sources
and ZIPs, without compiling anything or invoking the native signing backend.
--godot must select a final signed native Editor CLI. It exports small projects
through the actual C++ exporter, including default and custom template selection,
using the generated fixture ZIPs. Fixture libraries and exported PCKs are never
executed; there is no engine build, Hvigor, network, HDC or installation.
Every generated file lives in TMPDIR and is removed. Real HOME is preserved.
JSON receipts go to stdout; progress and unittest output go to stderr.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import zipfile

# Loading production Python helpers must not write generated files into source.
sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[3]
METADATA = 'entry/src/main/resources/rawfile/godot_template.json'
LIBRARY = 'entry/libs/arm64-v8a/libgodot.so'
MISSING_GLES = 'rebuild them with opengl3=yes'
MISSING_VULKAN = 'rebuild them with vulkan=yes'
INVALID_METADATA = 'invalid godot_template.json metadata'
INVALID_DRIVERS = 'invalid renderingDrivers metadata'
ELF = bytearray(64)
ELF[:6] = b'\x7fELF\x02\x01'
ELF[18:20] = (183).to_bytes(2, 'little')
ELF = bytes(ELF) + b'NON-EXECUTABLE TEMPLATE TEST DATA\n'


@contextmanager
def private_directory():
    temporary_root = Path(os.environ['TMPDIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='godot-template-rendering-', dir=temporary_root) as temporary:
        root = Path(temporary).resolve(strict=True)
        if root.parent != temporary_root or not root.name.startswith('godot-template-rendering-'):
            raise RuntimeError('Unexpected template rendering fixture directory')
        try:
            yield root
        finally:
            if root.resolve(strict=True) != root or root.parent != temporary_root:
                raise RuntimeError('Unexpected template rendering fixture cleanup path')


class BuildEnvironment(dict):
    """Only adapt SCons graph construction; never execute a build action."""
    def __init__(self, library, target, vulkan, opengl3):
        super().__init__(arch='arm64', target=target, generate_bundle=False,
                         module_mono_enabled=False, vulkan=vulkan, opengl3=opengl3)
        self.library = library
        self.dev_build = False
        self.debug_features = target == 'template_debug'

    def Clone(self):
        return self

    def SharedObject(self, sources):
        return sources

    def add_shared_library(self, name, objects):
        return [types.SimpleNamespace(abspath=str(self.library))]


def generate_bundle(root, target, vulkan, opengl3):
    """Run the complete production function with real staging and archive I/O."""
    root.mkdir(parents=True)
    library = root / 'libgodot.fixture.so'
    library.write_bytes(ELF)
    archives = root / 'archives'
    archives.mkdir()
    env = BuildEnvironment(library, target, vulkan, opengl3)

    def node(path):
        if path.startswith('#bin/'):
            resolved = archives / path.removeprefix('#bin/')
        elif path.startswith('#'):
            resolved = REPO / path.removeprefix('#')
        else:
            raise RuntimeError('Unexpected SCons path: ' + path)
        return types.SimpleNamespace(abspath=str(resolved))

    methods = types.ModuleType('methods')
    methods.print_warning = lambda message: None
    with mock.patch.dict(sys.modules, {'methods': methods}):
        sys.path.insert(0, str(REPO))
        try:
            namespace = runpy.run_path(str(REPO / 'platform/openharmony/SCsub'), init_globals={
                'Import': lambda name: None, 'env': env,
                'File': node, 'Dir': node, 'Glob': lambda pattern: [],
            })
        finally:
            sys.path.pop(0)
        bundle = namespace['generate_bundle']
        # Suppress only the native signer build boundary. Do not replace or
        # reimplement metadata generation, template staging or archive creation.
        bundle.__globals__['sys'] = types.SimpleNamespace(platform='fixture-no-signer', executable=sys.executable)
        bundle(None, None, env)
    mode = 'debug' if target == 'template_debug' else 'release'
    archive = archives / f'openharmony_{mode}_arm64-v8a.zip'
    with zipfile.ZipFile(archive) as generated:
        metadata = json.loads(generated.read(METADATA))
        if generated.read(LIBRARY) != ELF:
            raise AssertionError('Generated template did not preserve the fake ELF data')
        if metadata['target'] != target or metadata['architecture'] != 'arm64-v8a':
            raise AssertionError('Generated template lost target/architecture metadata')
        if metadata['monoEnabled'] is not False or metadata['nativeAotEnabled'] is not False:
            raise AssertionError('Rendering metadata changed the managed build flags')
    return archive, metadata


def generation_matrix(root):
    # Last row tests serialization when Vulkan is false; detect.py currently
    # rejects a GL-only engine build, so this row does not claim build support.
    inputs = [
        ('debug-dual', 'template_debug', True, True, ['vulkan', 'opengl3']),
        ('debug-vulkan', 'template_debug', True, False, ['vulkan']),
        ('release-dual', 'template_release', True, True, ['vulkan', 'opengl3']),
        ('release-vulkan', 'template_release', True, False, ['vulkan']),
        ('metadata-only-no-vulkan', 'template_debug', False, True, ['opengl3']),
    ]
    records, archives = [], {}
    for name, target, vulkan, opengl3, expected in inputs:
        archive, metadata = generate_bundle(root / name, target, vulkan, opengl3)
        if metadata.get('renderingDrivers') != expected:
            raise AssertionError(f'{name}: expected {expected}, got {metadata}')
        records.append(dict(name=name, target=target, vulkan=vulkan, opengl3=opengl3,
                            renderingDrivers=metadata['renderingDrivers'], passed=True))
        archives[name] = archive
        print('PASS generated ' + name, file=sys.stderr)
    return records, archives


class TemplateRenderingGenerationTest(unittest.TestCase):
    def test_production_bundle_metadata_tracks_build_flags(self):
        with private_directory() as root:
            records, _ = generation_matrix(root)
            self.assertEqual(len(records), 5)


def replace_metadata(original, destination, payload):
    with zipfile.ZipFile(original) as source, zipfile.ZipFile(destination, 'w') as changed:
        for entry in source.infolist():
            if entry.filename == METADATA:
                if payload is not None:
                    changed.writestr(entry, payload)
            else:
                changed.writestr(entry, source.read(entry.filename))
    return destination


def template_variants(root, archives):
    variants = {}
    for mode in ('debug', 'release'):
        dual, vulkan = archives[mode + '-dual'], archives[mode + '-vulkan']
        with zipfile.ZipFile(dual) as source:
            metadata = json.loads(source.read(METADATA))
        legacy = dict(metadata)
        legacy.pop('renderingDrivers')
        variants['dual', mode] = dual
        variants['vulkan', mode] = vulkan
        payloads = {
            'legacy': json.dumps(legacy).encode(),
            'gles-only': json.dumps(dict(metadata, renderingDrivers=['opengl3'])).encode(),
            'absent': None,
            'bad-json': b'{"renderingDrivers":',
            'bad-root': b'["vulkan", "opengl3"]',
            'bad-string': json.dumps(dict(metadata, renderingDrivers='opengl3')).encode(),
            'bad-tail': json.dumps(dict(metadata, renderingDrivers=['opengl3', False])).encode(),
            'bad-empty': json.dumps(dict(metadata, renderingDrivers=[])).encode(),
        }
        for name, payload in payloads.items():
            variants[name, mode] = replace_metadata(dual, root / f'{name}-{mode}.zip', payload)
    variants['gles-only', 'debug'] = archives['metadata-only-no-vulkan']
    return variants


def export_cases():
    # All projects use a different desktop/base method than their mobile target
    # so the capability gate must resolve actual export features.
    return [
        dict(name='installed-debug-dual-gles', mode='debug', template='dual', gl=True),
        dict(name='installed-release-dual-gles', mode='release', template='dual', gl=True),
        dict(name='installed-debug-legacy-gles', mode='debug', template='legacy', gl=True, error=MISSING_GLES),
        dict(name='installed-release-legacy-gles', mode='release', template='legacy', gl=True, error=MISSING_GLES),
        dict(name='installed-debug-legacy-vulkan', mode='debug', template='legacy', gl=False),
        dict(name='installed-release-legacy-forward-plus', mode='release', template='legacy', gl=False, method='forward_plus'),
        dict(name='installed-absent-metadata-vulkan', mode='debug', template='absent', gl=False),
        dict(name='installed-absent-metadata-gles', mode='debug', template='absent', gl=True, error=MISSING_GLES),
        dict(name='custom-legacy-gles', mode='debug', template='legacy', custom=True, installed='dual', gl=True, error=MISSING_GLES),
        dict(name='custom-dual-gles', mode='release', template='dual', custom=True, installed='legacy', gl=True),
        dict(name='custom-vulkan-only-gles', mode='debug', template='vulkan', custom=True, installed='dual', gl=True, error=MISSING_GLES),
        dict(name='custom-legacy-vulkan', mode='debug', template='legacy', custom=True, installed='dual', gl=False),
        dict(name='custom-gles-only-vulkan', mode='debug', template='gles-only', custom=True, installed='dual', gl=False, error=MISSING_VULKAN),
        dict(name='custom-gles-only-gles', mode='debug', template='gles-only', custom=True, installed='vulkan', gl=True),
        dict(name='invalid-json-gles', mode='debug', template='bad-json', custom=True, installed='dual', gl=True, error=INVALID_METADATA),
        dict(name='invalid-root-vulkan', mode='debug', template='bad-root', custom=True, installed='legacy', gl=False, error=INVALID_METADATA),
        dict(name='invalid-string-gles', mode='debug', template='bad-string', custom=True, installed='dual', gl=True, error=INVALID_DRIVERS),
        dict(name='invalid-array-tail-gles', mode='debug', template='bad-tail', custom=True, installed='dual', gl=True, error=INVALID_DRIVERS),
        dict(name='invalid-empty-vulkan', mode='debug', template='bad-empty', custom=True, installed='dual', gl=False, error=INVALID_DRIVERS),
    ]


def isolated_environment(root):
    environment = dict(os.environ)
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME',
                 'XDG_RUNTIME_DIR', 'GODOT_OHOS_DATA_DIR', 'GODOT_OHOS_CACHE_DIR', 'TMPDIR'):
        directory = root / name.lower()
        directory.mkdir(mode=0o700)
        environment[name] = str(directory)
    environment['PYTHONDONTWRITEBYTECODE'] = '1'
    return environment


def run_export_case(godot, root, variants, sdk, case):
    root.mkdir()
    environment = isolated_environment(root)
    environment['GODOT_OHOS_SDK_ROOT'] = str(sdk)
    # Exercise the actual default template pointer, not a custom path pretending
    # to be an installed template. A custom case installs the opposite capability.
    installed = case.get('installed', case['template'])
    debug, release = variants[installed, 'debug'], variants[installed, 'release']
    digest = hashlib.sha256(debug.read_bytes() + release.read_bytes()).hexdigest()
    template_root = Path(environment['GODOT_OHOS_DATA_DIR']) / 'export_templates'
    selection = template_root / digest
    selection.mkdir(parents=True)
    shutil.copyfile(debug, selection / 'openharmony_debug_arm64-v8a.zip')
    shutil.copyfile(release, selection / 'openharmony_release_arm64-v8a.zip')
    (selection / '.ready').write_text('fixture template selection\n')
    (template_root / 'current.json').write_text(json.dumps(dict(schemaVersion=1, sha256=digest)))
    project = root / 'project with spaces'
    project.mkdir()
    target_method = case.get('method', 'gl_compatibility' if case['gl'] else 'mobile')
    base_method = 'mobile' if case['gl'] else 'gl_compatibility'
    (project / 'project.godot').write_text(
        'config_version=5\n[application]\nconfig/name="Template rendering fixture"\n'
        'run/main_scene="res://main.tscn"\n[rendering]\n'
        f'renderer/rendering_method={json.dumps(base_method)}\n'
        f'renderer/rendering_method.mobile={json.dumps(target_method)}\n'
        'rendering_device/driver.openharmony="vulkan"\n'
        'gl_compatibility/driver="opengl3"\n'
        'textures/vram_compression/import_etc2_astc=true\n')
    (project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Fixture" type="Node"]\n')
    preset = ('[preset.0]\nname="OpenHarmony"\nplatform="OpenHarmony"\nrunnable=true\n'
              'advanced_options=true\nexport_filter="all_resources"\n'
              '[preset.0.options]\narchitectures/arm64=true\narchitectures/x86_64=false\n'
              'build/export_project_only=true\nbuild/override_project_dir=false\n'
              'build/sign=false\nbuild/bundle_id="org.oheco.templaterenderingfixture"\n')
    if case.get('custom'):
        for mode in ('debug', 'release'):
            preset += f'custom_template/{mode}={json.dumps(str(variants[case["template"], mode]))}\n'
    (project / 'export_presets.cfg').write_text(preset)
    output = root / 'Export result.hap'
    command = [str(godot), '--headless', '--path', str(project),
               '--export-' + case['mode'], 'OpenHarmony', str(output)]
    result = subprocess.run(command, cwd=root, env=environment, text=True,
                            capture_output=True, timeout=300)
    text = result.stdout + result.stderr
    pck = output.with_suffix('') / 'entry/src/main/resources/rawfile/template.pck'
    error = case.get('error')
    if error:
        passed = result.returncode == 1 and error in text and not pck.exists() and not output.exists()
    else:
        exported_library = output.with_suffix('') / LIBRARY
        passed = (result.returncode == 0 and 'SCRIPT ERROR:' not in text and
                  pck.is_file() and pck.stat().st_size > 0 and exported_library.is_file() and
                  exported_library.read_bytes() == ELF and not output.exists())
    record = dict(name=case['name'], passed=passed, exitCode=result.returncode,
                  expected='reject' if error else 'project-only export', pckCreated=pck.exists())
    if not passed:
        record['output'] = text[-12000:]
    print(('PASS ' if passed else 'FAIL ') + case['name'], file=sys.stderr)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--generation-only', action='store_true', help='Check actual SCsub bundle generation; real Editor export remains pending')
    mode.add_argument('--godot', type=Path, help='Final signed native Editor CLI with the capability gate compiled in')
    parser.add_argument('--jobs', type=int, choices=(1, 2, 3, 4), default=1, help='Independent Editor export processes (default: 1)')
    args = parser.parse_args()
    receipt = dict(test='template_rendering', passed=False, actualEditorExport='pending')
    root = None
    try:
        if args.godot and (sys.platform != 'ohos' or os.uname().machine != 'aarch64'):
            raise RuntimeError('Actual Editor export checks must run on native aarch64 HarmonyOS')
        godot = args.godot.resolve(strict=True) if args.godot else None
        with private_directory() as root:
            records, archives = generation_matrix(root / 'generation')
            receipt['generationCases'] = records
            if godot:
                sys.path.insert(0, str(REPO / 'platform/openharmony/tests'))
                sys.path.insert(0, str(REPO / 'platform/openharmony'))
                try:
                    from sdk_fixture import make_sdk
                    sdk = make_sdk(root / 'sdk metadata only')
                finally:
                    sys.path.pop(0)
                    sys.path.pop(0)
                variants_root = root / 'template variants'
                variants_root.mkdir()
                variants = template_variants(variants_root, archives)
                cases_root = root / 'exports'
                cases_root.mkdir()
                with ThreadPoolExecutor(max_workers=args.jobs) as executor:
                    receipt['exportCases'] = list(executor.map(
                        lambda case: run_export_case(godot, cases_root / case['name'], variants, sdk, case), export_cases()))
                receipt['actualEditorExport'] = 'passed' if all(case['passed'] for case in receipt['exportCases']) else 'failed'
                receipt['godot'] = str(godot)
            receipt['passed'] = all(case['passed'] for case in records) and (not godot or receipt['actualEditorExport'] == 'passed')
    except (OSError, ValueError, RuntimeError, AssertionError, subprocess.TimeoutExpired) as error:
        receipt['error'] = str(error)
    finally:
        if root is not None:
            receipt['temporaryFixtureRemoved'] = not root.exists()
    print(json.dumps(receipt, sort_keys=True))
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
