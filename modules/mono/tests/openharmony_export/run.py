#!/usr/bin/env python3
"""Offline native regression for OpenHarmony C# NativeAOT game export.

Run after rebuilding managed packages as 4.7.2-ohos.3 and native editor/templates.
Projects, settings and caches stay in private temporary copies; --output keeps
only evidence. Never install or personally sign a HAP, or modify SDK packages.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

GAME_LIBRARY = 'libgodot-csharp-game.so'
AOT_METADATA = '.godot/mono/openharmony_aot.json'
DEPENDENCIES = ('libicudata.so.78', 'libicuuc.so.78', 'libicui18n.so.78',
                'libcrypto.so.3', 'libssl.so.3', 'libc++_shared.so')


def pck_contents(data):
    magic, version, _, _, _, flags, base = struct.unpack_from('<6IQ', data)
    if magic != 0x43504447 or version not in (2, 3, 4) or flags & 1:
        raise RuntimeError('Expected an unencrypted native Godot PCK')
    position = struct.unpack_from('<Q', data, 32)[0] if version >= 3 else 96
    count, = struct.unpack_from('<I', data, position)
    position += 4
    files = {}
    for _ in range(count):
        length, = struct.unpack_from('<I', data, position)
        position += 4
        name = data[position:position + length].rstrip(b'\0').decode().removeprefix('res://')
        position += length
        offset, size = struct.unpack_from('<QQ', data, position)
        position += 16
        digest = data[position:position + 16]
        position += 16
        entry_flags, = struct.unpack_from('<I', data, position)
        position += 4
        if entry_flags or name in files:
            raise RuntimeError('Encrypted/deleted/duplicate PCK entry: ' + name)
        content = data[base + offset:base + offset + size]
        if len(content) != size or hashlib.md5(content).digest() != digest:
            raise RuntimeError('PCK entry checksum mismatch: ' + name)
        files[name] = content
    return files


def has_signature(data):
    if not data.startswith(b'\x7fELF'):
        return False
    if len(data) < 64 or data[4:6] != b'\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise RuntimeError('Expected native AArch64 ELF')
    offset, = struct.unpack_from('<Q', data, 40)
    size, count, names_index = struct.unpack_from('<HHH', data, 58)
    names_header = struct.unpack_from('<IIQQQQIIQQ', data, offset + names_index * size)
    names = data[names_header[4]:names_header[4] + names_header[5]]
    for index in range(count):
        header = struct.unpack_from('<IIQQQQIIQQ', data, offset + index * size)
        if names[header[0]:].split(b'\0', 1)[0] == b'.codesign' and header[5] > 0:
            return True
    return False


def audit_pck(data):
    files = pck_contents(data)
    assert json.loads(files[AOT_METADATA]) == {'schemaVersion': 1, 'mode': 'native-aot', 'library': GAME_LIBRARY}
    assert files['Smoke.cs'].strip() == b'', 'C# source stripping contract was lost'
    assert not any(name.startswith('.godot/mono/publish/') or name.endswith(('.dll', '.dbg', '.deps.json', '.runtimeconfig.json'))
                   or '.dotnet-publish-manifest' in name for name in files), 'Runtime/symbols leaked into PCK'
    assert not any(content.startswith(b'\x7fELF') for content in files.values()), 'Native code belongs in HAP libs'
    assert len(data) < 1024 * 1024, 'Small fixture PCK unexpectedly contains runtime payload'
    return {'pckEntries': len(files), 'pckSize': len(data), 'metadata': json.loads(files[AOT_METADATA])}


def audit_libraries(libraries):
    names = {path.name: path for path in libraries.iterdir() if path.is_file()}
    for name in (GAME_LIBRARY, *DEPENDENCIES):
        assert name in names, 'Native library absent: ' + name
        data = names[name].read_bytes()
        assert data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<HH', data, 16) == (3, 183), name
        assert has_signature(data), 'Unsigned native library: ' + name
    assert not any(name.endswith(('.dll', '.dbg', '.runtimeconfig.json', '.deps.json'))
                   or name in ('libcoreclr.so', 'libhostfxr.so', 'libclrjit.so') for name in names)
    return {'nativeFiles': len(names), 'gameSha256': hashlib.sha256(names[GAME_LIBRARY].read_bytes()).hexdigest(),
            'nativeSha256': {name: hashlib.sha256(names[name].read_bytes()).hexdigest() for name in (GAME_LIBRARY, *DEPENDENCIES)},
            'nativePayloadBytes': sum(names[name].stat().st_size for name in (GAME_LIBRARY, *DEPENDENCIES))}


def audit_project(directory, mode):
    raw = directory / 'entry/src/main/resources/rawfile'
    template = json.loads((raw / 'godot_template.json').read_text())
    assert template['monoEnabled'] is True and template['nativeAotEnabled'] is True
    assert template['target'] == 'template_' + mode and template['architecture'] == 'arm64-v8a'
    host = json.loads((raw / 'godot_host.json').read_text())
    assert host['host']['role'] == 'game' and host['managed']['mode'] == 'none'
    module = json.loads((directory / 'entry/src/main/module.json5').read_text())['module']
    permissions = [entry['name'] for entry in module.get('requestPermissions', [])]
    assert 'ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY' not in permissions
    assert 'ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY' not in permissions
    assert module['deviceTypes'] == ['default']
    profile = json.loads((directory / 'build-profile.json5').read_text())['app']
    assert profile.get('signingConfigs', []) == []
    assert all(not product.get('signingConfig') for product in profile['products'])
    app = json.loads((directory / 'AppScope/app.json5').read_text())['app']
    assert app['bundleName'] == 'org.oheco.csharpgamewithspaces'
    assert not list(raw.rglob('runtime.zip')) and not list(raw.rglob('runtime-manifest.json'))
    pack = raw / 'template.pck'
    return {'mode': mode, 'permissions': permissions,
            'pckSha256': hashlib.sha256(pack.read_bytes()).hexdigest(),
            **audit_pck(pack.read_bytes()), **audit_libraries(directory / 'entry/libs/arm64-v8a')}


def audit_bundle(path, project_record):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        packs = [name for name in names if name.endswith('/rawfile/template.pck')]
        assert len(packs) == 1
        pack = archive.read(packs[0])
        assert hashlib.sha256(pack).hexdigest() == project_record['pckSha256']
        native_entry = 'libs/arm64-v8a/' + GAME_LIBRARY
        native = [name for name in names if name == native_entry or name.endswith('/' + native_entry)]
        assert len(native) == 1, 'AOT game library was not packaged in HAP libs'
        for dependency in (GAME_LIBRARY, *DEPENDENCIES):
            entry = 'libs/arm64-v8a/' + dependency
            matching = [name for name in names if name == entry or name.endswith('/' + entry)]
            assert len(matching) == 1 and has_signature(archive.read(matching[0])), dependency
            assert hashlib.sha256(archive.read(matching[0])).hexdigest() == project_record['nativeSha256'][dependency], 'Packaged native bytes changed: ' + dependency
        assert not any(name.endswith(('.dbg', '.dll', 'runtime.zip', '.dotnet-publish-manifest')) for name in names)
        audit = audit_pck(pack)
    return {'path': str(path), 'signed': False, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), **audit}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('dotnet-sdk', 'nuget-feed', 'godotsharp', 'native-sdk', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--editor', type=Path)
    parser.add_argument('--restricted-tool-path', action='store_true')
    parser.add_argument('--editor-sdk-root', type=Path)
    parser.add_argument('--template-debug', type=Path)
    parser.add_argument('--template-release', type=Path)
    parser.add_argument('--hap-template-debug', type=Path)
    parser.add_argument('--hap-template-release', type=Path)
    parser.add_argument('--build-bundles', action='store_true', help='Build unsigned HAPs; never install or personally sign them')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Run acceptance on native OpenHarmony')
    full = args.hap_template_debug or args.hap_template_release
    if (full or args.restricted_tool_path or args.editor_sdk_root) and not args.editor:
        parser.error('Editor options require --editor')
    if args.build_bundles and not full:
        parser.error('--build-bundles requires a HAP template')
    if args.editor_sdk_root and (not args.editor_sdk_root.is_absolute() or not args.editor_sdk_root.is_dir()):
        parser.error('--editor-sdk-root must be an existing absolute SDK view root')
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    source = Path(__file__).resolve().parents[4]
    sharp, sdk, native_sdk = args.godotsharp.resolve(), args.dotnet_sdk.resolve(), args.native_sdk.resolve()
    signer = shutil.which('binary-sign-tool')
    if not signer:
        raise RuntimeError('Native publish requires the installed binary-sign-tool on PATH')
    checks, records = [], []
    with tempfile.TemporaryDirectory(prefix='godot-csharp-aot-export-', dir=os.environ['TMPDIR']) as private_name:
        private = Path(private_name)
        project, feed = private / 'CSharp game with spaces', private / 'feed'
        shutil.copytree(Path(__file__).with_name('fixture'), project)
        feed.mkdir()
        for root in (args.nuget_feed, sharp / 'Tools/nupkgs', sdk / 'library-packs'):
            for package in root.glob('*.nupkg'):
                shutil.copyfile(package, feed / package.name)
        for package in ('Godot.NET.Sdk', 'GodotSharp', 'Godot.SourceGenerators'):
            if not any(path.name.lower() == (package + '.4.7.2-ohos.3.nupkg').lower() for path in feed.iterdir()):
                raise RuntimeError('Rebuild local 4.7.2-ohos.3 managed packages before testing: ' + package)
        env = dict(os.environ)
        for key in ('LD_LIBRARY_PATH', 'LD_PRELOAD'):
            env.pop(key, None)
        (private / 'tmp').mkdir()
        env.update(DOTNET_ROOT=str(sdk), DOTNET_ROOT_ARM64=str(sdk), DOTNET_CLI_HOME=str(private / 'cli'),
                   DOTNET_OHOS_TMPDIR=str(private / 'tmp'), TMPDIR=str(private / 'tmp'),
                   NUGET_PACKAGES=str(private / 'nuget'), NUGET_SCRATCH=str(private / 'nuget-scratch'),
                   NUGET_HTTP_CACHE_PATH=str(private / 'nuget-http'), GODOT_NUGET_SOURCE=str(feed),
                   GODOT_SHARP_ROOT=str(sharp), GODOT_OHOS_DATA_DIR=str(private / 'data'),
                   GODOT_OHOS_CACHE_DIR=str(private / 'cache'), DOTNET_CLI_TELEMETRY_OPTOUT='1',
                   DOTNET_GENERATE_ASPNET_CERTIFICATE='false', DOTNET_CLI_USE_MSBUILD_SERVER='0',
                   MSBUILDDISABLENODEREUSE='1', DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true',
                   NuGetAudit='false', UseSharedCompilation='false',
                   PATH=str(native_sdk / 'llvm/bin') + os.pathsep + os.environ['PATH'])
        Path(env['GODOT_OHOS_DATA_DIR']).mkdir()
        Path(env['GODOT_OHOS_CACHE_DIR']).mkdir()

        def run(name, command, marker=None, run_env=None, cwd=None, expected_exit=0, timeout=600):
            with (output / (name + '.log')).open('w') as log:
                log.write('+ ' + ' '.join(map(str, command)) + '\n'); log.flush()
                completed = subprocess.run(list(map(str, command)), cwd=cwd or project, env=run_env or env,
                                           stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            if completed.returncode != expected_exit or (marker and marker not in (output / (name + '.log')).read_text()):
                raise RuntimeError(name + ' failed; see its evidence log')
            checks.append(name)

        dotnet = sdk / 'dotnet'
        reference_fixture = private / 'ScriptLibrary.dll'
        reference_fixture.write_bytes(b'resolve-only project reference fixture; not executed')
        for configuration in ('Debug', 'ExportDebug', 'ExportRelease'):
            run('sdk-defaults-' + configuration, [dotnet, 'msbuild', Path(__file__).with_name('SdkTests.proj'),
                '-p:Configuration=' + configuration, '-p:ProjectReferenceFixturePath=' + str(reference_fixture)],
                'OHOS_NATIVEAOT_SDK_PROBE_PASS')
        for editor_assembly in ('GodotSharpEditor.dll', 'GodotTools.BuildLogger.dll'):
            run('sdk-editor-reference-rejected-' + editor_assembly, [dotnet, 'msbuild', Path(__file__).with_name('SdkTests.proj'),
                '-p:Configuration=ExportRelease', '-p:ProjectReferenceFixturePath=' + str(reference_fixture),
                '-p:EditorReference=true', '-p:EditorReferenceName=' + editor_assembly], marker='editor-only assembly', expected_exit=1)
        run('solution-create', [dotnet, 'new', 'sln', '--format', 'sln', '--name', 'Smoke'])
        run('solution-add', [dotnet, 'sln', 'Smoke.sln', 'add', 'Smoke.csproj'])
        publish = private / 'native-aot publish'
        run('native-aot-publish', [dotnet, 'publish', 'Smoke.csproj', '-c', 'ExportRelease',
            '-r', 'openharmony-arm64', '--self-contained', 'true', '-o', publish,
            '-p:GodotTargetPlatform=openharmony', '-p:PublishAot=true', '-p:NativeLib=Shared',
            '-p:CppCompilerAndLinker=' + str(native_sdk / 'llvm/bin/clang'),
            '-p:OpenHarmonySigningTool=' + str(Path(signer).resolve()),
            '-p:TrimmerSingleWarn=false', '-p:DebugType=None', '-p:DebugSymbols=false',
            '-p:NuGetAudit=false', '-p:UseSharedCompilation=false'])
        assert not list(publish.glob('*.dll')), 'NativeAOT cannot silently publish CoreCLR instead'
        libraries = private / 'native-libraries'
        libraries.mkdir()
        for artifact in publish.iterdir():
            if artifact.is_file() and not artifact.name.endswith('.dbg') and (artifact.name.endswith('.so') or '.so.' in artifact.name):
                shutil.copy2(artifact, libraries / (GAME_LIBRARY if artifact.name == 'Smoke.so' else artifact.name))
        native_audit = audit_libraries(libraries)
        shutil.copytree(libraries, output / 'native-libraries')
        helper = private / 'helper'
        helper.mkdir()
        shutil.copyfile(source / 'modules/mono/editor/GodotTools/GodotTools/Export/OpenHarmonyExport.cs', helper / 'OpenHarmonyExport.cs')
        shutil.copyfile(Path(__file__).with_name('HelperTests.cs'), helper / 'HelperTests.cs')
        (helper / 'Helper.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><OutputType>Exe</OutputType><Nullable>enable</Nullable></PropertyGroup></Project>')
        (helper / 'NuGet.Config').write_text('<configuration><packageSources><clear/></packageSources></configuration>')
        run('helper-build', [dotnet, 'build', helper / 'Helper.csproj', '-p:OpenHarmonyCodesignEnabled=false', '-p:UseSharedCompilation=false', '-p:NuGetAudit=false'])
        run('helper-regression', [dotnet, helper / 'bin/Debug/net10.0/Helper.dll', private / 'helper-fixture', publish], 'OHOS_EXPORT_HELPER_PASS')
        if args.editor:
            editor, editor_env = args.editor.resolve(), dict(env)
            if args.restricted_tool_path:
                editor_env['PATH'] = '/system/bin'
                editor_env.pop('OpenHarmonySigningTool', None)
                editor_env.pop('CppCompilerAndLinker', None)
            if args.editor_sdk_root:
                run('editor-version', [editor, '--version'], run_env=editor_env)
                match = re.search(r'^(\d+)\.(\d+)\.', (output / 'editor-version.log').read_text(), re.MULTILINE)
                if not match: raise RuntimeError('Cannot determine EditorSettings version')
                config = Path(editor_env['GODOT_OHOS_DATA_DIR']) / 'config/godot'
                config.mkdir(parents=True)
                (config / ('editor_settings-' + '.'.join(match.groups()) + '.tres')).write_text('[gd_resource type="EditorSettings" format=3]\n\n[resource]\nexport/openharmony/sdk_root = ' + json.dumps(str(args.editor_sdk_root.resolve())) + '\n')
            run('editor-import', [editor, '--headless', '--editor', '--path', project, '--quit'], run_env=editor_env)
            preset = project / 'export_presets.cfg'
            preset_text = '''[preset.0]
name="CSharp OpenHarmony"
platform="OpenHarmony"
runnable=true
advanced_options=true
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path=""
script_export_mode=2
[preset.0.options]
architectures/arm64=true
architectures/x86_64=false
dotnet/embed_build_outputs=false
dotnet/include_scripts_content=false
dotnet/include_debug_symbols=false
'''
            preset.write_text(preset_text)
            for mode, template in (('debug', args.template_debug), ('release', args.template_release)):
                pack = output / ('game-' + mode + '.pck')
                command = [editor, '--headless', '--path', project, '--export-pack', 'CSharp OpenHarmony', pack]
                if mode == 'debug':
                    command = [editor, '--headless', '--path', project, '--export-debug', 'CSharp OpenHarmony', '--export-pack', 'CSharp OpenHarmony', pack]
                run('pck-export-' + mode, command, run_env=editor_env)
                records.append({'mode': mode, **audit_pck(pack.read_bytes())})
                if template:
                    game_env = dict(env)
                    for key in ('GODOT_SHARP_ROOT', 'GODOT_NUGET_SOURCE', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME', 'DOTNET_OHOS_TMPDIR', 'OHECO_ROOT'):
                        game_env.pop(key, None)
                    game_env.update(DOTNET_ROOT='/no-external-sdk', DOTNET_ROOT_ARM64='/no-external-sdk', DOTNET_MULTILEVEL_LOOKUP='0', PATH='/system/bin')
                    game_directory = private / ('template-game-' + mode)
                    shutil.copytree(libraries, game_directory)
                    executable = game_directory / 'godot'
                    shutil.copy2(template.resolve(), executable)
                    shutil.copy2(template.resolve().with_name('libgodot.so'), game_directory / 'libgodot.so')
                    shutil.copyfile(pack, game_directory / 'godot.pck')
                    for attempt in ('first', 'second'):
                        run('template-' + mode + '-' + attempt, [executable, '--headless', '--verbose', '--', '--verify'], 'OHOS_CSHARP_SMOKE_PASS', game_env, game_directory)
                        assert 'GODOT_NATIVEAOT_RUNTIME_CONFIRMED' in (output / ('template-' + mode + '-' + attempt + '.log')).read_text()
                    assert not list(Path(game_env['GODOT_OHOS_CACHE_DIR']).rglob('.dotnet-publish-manifest'))
            if full:
                for mode, archive in (('debug', args.hap_template_debug), ('release', args.hap_template_release)):
                    if not archive: continue
                    extra = '\nbuild/export_project_only=true\nbuild/sdk_version="26.0.0"\nbuild/compatible_api=23\nbuild/bundle_id="org.oheco.csharpgamewithspaces"\nbuild/sign=false\ncustom_template/' + mode + '=' + json.dumps(str(archive.resolve())) + '\n'
                    preset.write_text(preset_text + extra)
                    target = private / ('full-export-' + mode + '.hap')
                    run('full-project-export-' + mode, [editor, '--headless', '--path', project, '--export-' + mode, 'CSharp OpenHarmony', target], run_env=editor_env)
                    assert not target.exists()
                    record = audit_project(target.with_suffix(''), mode)
                    shutil.copytree(target.with_suffix(''), output / ('full-export-' + mode))
                    if args.build_bundles:
                        preset.write_text((preset_text + extra).replace('build/export_project_only=true', 'build/export_project_only=false'))
                        bundle = output / ('unsigned-' + mode + '.hap')
                        run('unsigned-hap-export-' + mode, [editor, '--headless', '--path', project, '--export-' + mode, 'CSharp OpenHarmony', bundle], run_env=editor_env, timeout=1200)
                        record['unsignedBundle'] = audit_bundle(bundle, record)
                    records.append(record)
                # Test a real resolved editor reference, not a copied output DLL
                # that AOT would discard before the export validator could see it.
                csproj = project / 'Smoke.csproj'
                original = csproj.read_text()
                editor_api = sharp / 'Api/Debug/GodotSharpEditor.dll'
                assert editor_api.is_file()
                try:
                    csproj.write_text(original.replace('</Project>', '<ItemGroup><Reference Include="GodotSharpEditor"><HintPath>' + str(editor_api) + '</HintPath></Reference></ItemGroup></Project>'))
                    failed = output / 'expected-failed-publish.hap'
                    mode, archive = next((mode, archive) for mode, archive in
                        (('debug', args.hap_template_debug), ('release', args.hap_template_release)) if archive)
                    # Headless editor stdout only reports the failed build; the
                    # MSBuild panel retains its detailed SDK diagnostic. Require
                    # the same real reference graph to fail directly as well.
                    run('publish-editor-reference-rejected', [dotnet, 'publish', 'Smoke.csproj',
                        '-c', 'ExportDebug' if mode == 'debug' else 'ExportRelease',
                        '-r', 'openharmony-arm64', '--self-contained', 'true', '-o', private / 'rejected-native-publish',
                        '-p:GodotTargetPlatform=openharmony', '-p:PublishAot=true', '-p:NativeLib=Shared',
                        '-p:NuGetAudit=false', '-p:UseSharedCompilation=false'],
                        marker='editor-only assembly', expected_exit=1)
                    assert not list((private / 'rejected-native-publish').glob('*.so'))
                    negative_options = '\nbuild/export_project_only=true\nbuild/sdk_version="26.0.0"\nbuild/compatible_api=23\nbuild/bundle_id="org.oheco.csharpgamewithspaces"\nbuild/sign=false\ncustom_template/' + mode + '=' + json.dumps(str(archive.resolve())) + '\n'
                    preset.write_text(preset_text + negative_options)
                    run('full-export-editor-reference-rejected', [editor, '--headless', '--verbose', '--path', project, '--export-' + mode, 'CSharp OpenHarmony', failed], marker='Failed to build project', run_env=editor_env, expected_exit=1)
                    build_logs = list(private.rglob('msbuild_log.txt'))
                    assert any('editor-only assembly' in log.read_text(errors='replace') for log in build_logs), 'Editor MSBuild diagnostic was not captured'
                    for index, log in enumerate(build_logs):
                        shutil.copy2(log, output / ('negative-editor-msbuild-' + str(index) + '.log'))
                    # Native export may create an incomplete template directory;
                    # rejection must not create a HAP or deploy the game library.
                    assert not failed.exists() and not (failed.with_suffix('') / 'entry/libs/arm64-v8a' / GAME_LIBRARY).exists()
                finally:
                    csproj.write_text(original)
    (output / 'result.json').write_text(json.dumps({'platform': sys.platform, 'passed': checks,
        'nativeAudit': native_audit, 'exports': records, 'restrictedToolPath': args.restricted_tool_path,
        'scope': 'Native terminal/headless acceptance; no personal HAP signing, installation or GUI/HAP sandbox execution.'}, indent=2) + '\n')
    print('OpenHarmony NativeAOT game export acceptance passed:', output)


if __name__ == '__main__':
    main()
