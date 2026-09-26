#!/usr/bin/env python3
"""Assemble a complete, relocatable Godot .NET Editor DevEco project offline."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat
import zipfile


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


# DevEco owns these files once the project has been opened: signing
# configuration, resolved dependency locks and local overrides. An in-place
# update must keep them, otherwise the user has to reconfigure signing after
# every new export.
PROTECTED = {
    '.clang-tidy',
    '.clangd',
    'build-profile.json5',
    'entry/build-profile.json5',
    'oh-package-lock.json5',
    'entry/oh-package-lock.json5',
    'local.properties',
}


def files(root, project=False):
    """Walk logical paths, including in-tree directory links, without losing SDK packs."""
    boundary = root.resolve(strict=True)
    excluded = {'.hvigor', '.cxx', 'node_modules', 'oh_modules', 'build', '.git'}

    def walk(directory, ancestors):
        resolved = directory.resolve(strict=True)
        if not resolved.is_relative_to(boundary):
            raise ValueError(f'External symlink is not a reproducible input: {directory}')
        if resolved in ancestors:
            raise ValueError(f'Symlink directory cycle: {directory}')
        for path in sorted(directory.iterdir()):
            if project and path.name in excluded:
                continue
            target = path.resolve(strict=True)
            if not target.is_relative_to(boundary):
                raise ValueError(f'External symlink is not a reproducible input: {path}')
            if path.is_dir():
                yield from walk(path, ancestors | {resolved})
            elif path.is_file():
                yield path
            else:
                raise ValueError(f'Unsupported runtime entry: {path}')

    yield from walk(root, set())


def zip_tree(archive, root, prefix=''):
    for source in files(root):
        relative = source.relative_to(root)
        # files() validates both file links and every ancestor directory link.
        info = zipfile.ZipInfo((Path(prefix) / relative).as_posix(), date_time=(2026, 9, 12, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (stat.S_IFREG | (source.stat().st_mode & 0o777)) << 16
        with source.open('rb') as reader, archive.open(info, 'w', force_zip64=True) as writer:
            shutil.copyfileobj(reader, writer, length=1024 * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True, help='Final ARM64 libgodot.so; do not strip after signing')
    parser.add_argument('--godotsharp', type=Path, required=True)
    parser.add_argument('--dotnet-sdk', type=Path, required=True, help='Build input: the native OpenHarmony .NET SDK used for GodotSharp; it is not shipped')
    parser.add_argument('--nuget-feed', type=Path, required=True, help='Verified pinned NuGet archives')
    parser.add_argument('--output', type=Path, required=True, help='DevEco project directory')
    parser.add_argument('--archive', type=Path, help='Optional project ZIP')
    parser.add_argument('--update', action='store_true',
                        help='Update an existing project directory in place, preserving DevEco-owned files')
    parser.add_argument('--load-independent-library', action='store_true',
                        help='Declare ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY (restricted, needs an ACL '
                             'in the signing profile); only then can the application load the .NET runtime')
    parser.add_argument('--writable-code-memory', action='store_true',
                        help='Declare ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY (restricted, needs an ACL '
                             'in the signing profile); only then can the .NET runtime create writable code memory')
    parser.add_argument('--custom-sandbox', action='store_true',
                        help='Declare ohos.permission.CUSTOM_SANDBOX (restricted, needs an ACL in the signing '
                             'profile). A weak sandbox is what lets the application start adhoc-signed executables '
                             'from the user directory, such as the oo-installed .NET host')
    args = parser.parse_args()
    if (args.output.exists() and not args.update) or (args.archive and args.archive.exists()):
        raise SystemExit('Refusing to overwrite an existing project or archive; pass --update to refresh a project in place')
    source = Path(__file__).resolve().parents[2]
    template = source / 'misc/dist/openharmony_editor'
    # The packaged OpenHarmony .NET SDK records its provenance as
    # BUILDINFO.aspnetcore.json; an in-tree SDK build uses BUILDINFO.json.
    dotnet_buildinfo_path = next(
        (candidate for candidate in (args.dotnet_sdk / 'BUILDINFO.json',
                                     args.dotnet_sdk / 'BUILDINFO.aspnetcore.json')
         if candidate.is_file()), None)
    if dotnet_buildinfo_path is None:
        raise SystemExit('Missing native .NET SDK provenance (BUILDINFO.json or BUILDINFO.aspnetcore.json)')
    dotnet_buildinfo = json.loads(dotnet_buildinfo_path.read_text())
    if dotnet_buildinfo.get('rid') != 'openharmony-arm64':
        raise SystemExit(f'Expected .NET RID openharmony-arm64, found {dotnet_buildinfo.get("rid")!r}')
    required = [(args.library, 'native Godot editor'),
                (args.library.parent / 'build-info.json', 'native editor build provenance from build-cli.py'),
                (args.dotnet_sdk / 'dotnet', 'native .NET SDK host'),
                (args.godotsharp / 'Api/Debug/GodotSharp.dll', 'GodotSharp API'),
                (args.godotsharp / 'Tools/GodotTools.dll', 'Godot C# editor tools')]
    for path, description in required:
        if not path.is_file():
            raise SystemExit(f'Missing {description}: {path}')
    for name in ('sdk', 'host/fxr', 'shared/Microsoft.NETCore.App',
                 'packs/Microsoft.NETCore.App.Ref',
                 'packs/Microsoft.NETCore.App.Runtime.openharmony-arm64'):
        if not (args.dotnet_sdk / name).is_dir():
            raise SystemExit(f'Missing native SDK directory: {name}')
    # The SDK is only a build input and the version the application must find at
    # runtime; it is not part of the project, so validate it but do not walk it.
    for path in (args.library, args.dotnet_sdk / 'dotnet'):
        with path.open('rb') as stream:
            header = stream.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
            raise SystemExit(f'Expected ELF64 AArch64: {path}')
    library_digest = sha256(args.library)
    native_info = json.loads((args.library.parent / 'build-info.json').read_text())
    if native_info['libgodot.so_sha256'] != library_digest:
        raise SystemExit('Native build provenance does not match the provided library')
    manifest = json.loads((source / 'platform/openharmony/dotnet/nuget-inputs.json').read_text())
    for item in manifest['packages']:
        path = args.nuget_feed / item['archive']
        if not path.is_file() or path.stat().st_size != item['size'] or sha256(path) != item['sha256']:
            raise SystemExit(f'NuGet input mismatch: {path}')
    if not list((args.godotsharp / 'Tools/nupkgs').glob('Godot.NET.Sdk.*.nupkg')):
        raise SystemExit('GodotSharp/Tools/nupkgs must contain the adapted Godot.NET.Sdk package')
    args.output.mkdir(parents=True, exist_ok=True)
    generated = ('entry/libs/', 'entry/src/main/cpp/include/', 'entry/src/main/resources/rawfile/')
    preserved = []
    for path in files(template, project=True):
        name = path.relative_to(template).as_posix()
        if name.startswith(generated) or name in ('.gitignore', 'local.properties') or path.suffix in ('.p12', '.p7b', '.cer'):
            continue
        target = args.output / name
        if name in PROTECTED and target.exists():
            preserved.append(name)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    # The ArkTS tree belongs entirely to the template: drop files the template no
    # longer has, otherwise a renamed or removed page stays behind in the project
    # DevEco compiles.
    arkts = 'entry/src/main/ets'
    expected = {p.relative_to(template / arkts).as_posix()
                for p in (template / arkts).rglob('*') if p.is_file()}
    exported_arkts = args.output / arkts
    if exported_arkts.is_dir():
        for path in sorted(exported_arkts.rglob('*'), reverse=True):
            if path.is_file() and path.relative_to(exported_arkts).as_posix() not in expected:
                path.unlink()
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()
    # The native tree is template-owned too, except for include/, which is
    # generated below from the platform headers. Without this, a source the
    # template no longer ships stays in the project and keeps confusing the build.
    native = 'entry/src/main/cpp'
    expected_native = {p.relative_to(template / native).as_posix()
                       for p in (template / native).rglob('*') if p.is_file()}
    exported_native = args.output / native
    if exported_native.is_dir():
        for path in sorted(exported_native.rglob('*'), reverse=True):
            relative = path.relative_to(exported_native).as_posix()
            if path.is_file():
                if relative not in expected_native and not relative.startswith('include/'):
                    path.unlink()
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()
    restricted = []
    if args.load_independent_library:
        restricted.append(('ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY',
                           'HarmonyOS loads a library outside the application\n'
                           '      // bundle only from a directory the process registered with the linker.'))
    if args.writable_code_memory:
        restricted.append(('ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY',
                           'The .NET runtime needs writable code\n'
                           '      // memory: its JIT emits code and it patches the GC write barrier.'))
    if args.custom_sandbox:
        restricted.append(('ohos.permission.CUSTOM_SANDBOX',
                           'A weak sandbox is what lets the application\n'
                           '      // start adhoc-signed executables from the user directory, such as the\n'
                           '      // oo-installed .NET host.'))
    if restricted:
        # These are restricted permissions: without an ACL entry for them in the
        # signing profile the installation fails with "grant request permissions
        # failed", so only declare the ones that were actually granted.
        module = args.output / 'entry/src/main/module.json5'
        text = module.read_text()
        anchor = '"requestPermissions": ['
        if anchor not in text:
            raise SystemExit(f'Cannot find the permission list in {module}')
        declared = []
        for name, comment in restricted:
            if name in text:
                continue
            # The entries belong inside the array, and the anchor is the line that
            # opens it, so the replacement keeps the anchor first.
            text = text.replace(anchor, anchor + f'\n      // Restricted (ACL): {comment}\n      {{ "name": "{name}" }},', 1)
            declared.append(name)
        if declared:
            module.write_text(text)
            print('Declared ' + ', '.join(declared))
    native = args.output / 'entry/libs/arm64-v8a'
    native.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.library, native / 'libgodot.so')
    include = args.output / 'entry/src/main/cpp/include'
    include.mkdir(parents=True, exist_ok=True)
    for name in ('bridge_openharmony.h', 'editor_bridge_openharmony.h'):
        shutil.copyfile(source / 'platform/openharmony' / name, include / name)
    raw = args.output / 'entry/src/main/resources/rawfile'
    raw.mkdir(parents=True, exist_ok=True)
    runtime = raw / 'runtime.zip'
    with zipfile.ZipFile(runtime, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        # The .NET SDK is deliberately not shipped: the application resolves it
        # from the oheco package installation so it stays decoupled from the
        # SDK version. Only Godot's own managed assemblies and the pinned feed
        # travel with the project.
        zip_tree(archive, args.godotsharp, 'GodotSharp')
        zip_tree(archive, source / 'platform/openharmony/tests/dotnet-smoke', 'Examples/CSharpSmoke')
        for item in manifest['packages']:
            archive.write(args.nuget_feed / item['archive'], 'nuget/' + item['archive'])
        for package in sorted((args.godotsharp / 'Tools/nupkgs').glob('*.nupkg')):
            archive.write(package, 'nuget/' + package.name)
        # Pin the timestamp of the generated metadata entries too: writestr()
        # would otherwise stamp the current time and make an otherwise identical
        # runtime archive unreproducible.
        requirement = {
            'rid': dotnet_buildinfo.get('rid'),
            'sdk_version': dotnet_buildinfo.get('sdk_version'),
            'runtime_version': dotnet_buildinfo.get('runtime_version'),
            'resolution': "oheco package directory ('oo install dotnet-sdk')",
            'override_environment': 'GODOT_OHOS_DOTNET_ROOT',
        }
        metadata = (
            ('NuGet.Config', '<configuration><packageSources><clear/><add key="bundled" value="nuget"/></packageSources></configuration>\n'),
            ('nuget-inputs.json', json.dumps(manifest, indent=2) + '\n'),
            ('dotnet-requirement.json', json.dumps(requirement, indent=2) + '\n'),
        )
        for name, text in metadata:
            info = zipfile.ZipInfo(name, date_time=(2026, 9, 12, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, text)
    runtime_manifest = {'version': '4.7.2-ohos.2', 'sha256': sha256(runtime), 'size': runtime.stat().st_size}
    (raw / 'runtime-manifest.json').write_text(json.dumps(runtime_manifest, indent=2) + '\n')
    notices = args.output / 'licenses'
    notices.mkdir(exist_ok=True)
    for name in ('LICENSE.txt', 'COPYRIGHT.txt', 'AUTHORS.md'):
        shutil.copyfile(source / name, notices / name)
    vulkan_inputs = json.loads((source / 'thirdparty/vulkan/openharmony-inputs.json').read_text())
    for item in vulkan_inputs['archives']:
        target = notices / 'vulkan-dependencies' / item['name'] / Path(item['license_file']).name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / item['license_file'], target)
    for path in files(source / 'thirdparty/vulkan/LICENSES'):
        target = notices / 'vulkan-dependencies/Vulkan-Headers/LICENSES' / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    (notices / 'vulkan-dependencies/inputs.json').write_text(json.dumps(vulkan_inputs, indent=2) + '\n')
    provenance = {'upstream': 'Godot 4.7.2-stable', 'adaptation': '4.7.2-ohos.2',
                  'architecture': 'aarch64-linux-ohos', 'libgodot_sha256': library_digest,
                  'runtime': runtime_manifest,
                  'dotnet_resolution': "resolved at runtime from the oheco package installation; not shipped"}
    provenance['dotnet_buildinfo'] = dotnet_buildinfo
    provenance['native_build'] = native_info
    provenance['tool_execution'] = {
        'scope': 'in-process and direct child processes of the editor',
        'requires': 'ohos.permission.CUSTOM_SANDBOX (weak sandbox) to start the adhoc .NET host',
    }
    (args.output / 'build-inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            zip_tree(archive, args.output, args.output.name)
        args.archive.with_suffix(args.archive.suffix + '.sha256').write_text(sha256(args.archive) + '  ' + args.archive.name + '\n')
    if preserved:
        print('Preserved DevEco-owned files: ' + ', '.join(sorted(preserved)))
    print(args.output)


if __name__ == '__main__':
    main()
