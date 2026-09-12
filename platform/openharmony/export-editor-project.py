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
    parser.add_argument('--dotnet-sdk', type=Path, required=True, help='Complete signed native OpenHarmony SDK layout')
    parser.add_argument('--nuget-feed', type=Path, required=True, help='Verified pinned NuGet archives')
    parser.add_argument('--output', type=Path, required=True, help='New DevEco project directory')
    parser.add_argument('--archive', type=Path, help='Optional project ZIP')
    args = parser.parse_args()
    if args.output.exists() or (args.archive and args.archive.exists()):
        raise SystemExit('Refusing to overwrite an existing project or archive')
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
    # Validate links before creating output. ZIP entries are regular files, so
    # both DevEco extraction and the in-app extractor get a relocatable layout.
    sdk_files = list(files(args.dotnet_sdk))
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
    args.output.mkdir(parents=True, exist_ok=False)
    generated = ('entry/libs/', 'entry/src/main/cpp/include/', 'entry/src/main/resources/rawfile/')
    for path in files(template, project=True):
        name = path.relative_to(template).as_posix()
        if name.startswith(generated) or name in ('.gitignore', 'local.properties') or path.suffix in ('.p12', '.p7b', '.cer'):
            continue
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    native = args.output / 'entry/libs/arm64-v8a'
    native.mkdir(parents=True)
    shutil.copyfile(args.library, native / 'libgodot.so')
    include = args.output / 'entry/src/main/cpp/include'
    include.mkdir(parents=True)
    for name in ('bridge_openharmony.h', 'editor_bridge_openharmony.h'):
        shutil.copyfile(source / 'platform/openharmony' / name, include / name)
    raw = args.output / 'entry/src/main/resources/rawfile'
    raw.mkdir(parents=True)
    runtime = raw / 'runtime.zip'
    with zipfile.ZipFile(runtime, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        zip_tree(archive, args.dotnet_sdk, 'dotnet')
        zip_tree(archive, args.godotsharp, 'GodotSharp')
        zip_tree(archive, source / 'platform/openharmony/tests/dotnet-smoke', 'Examples/CSharpSmoke')
        for item in manifest['packages']:
            archive.write(args.nuget_feed / item['archive'], 'nuget/' + item['archive'])
        for package in sorted((args.godotsharp / 'Tools/nupkgs').glob('*.nupkg')):
            archive.write(package, 'nuget/' + package.name)
        archive.writestr('NuGet.Config', '<configuration><packageSources><clear/><add key="bundled" value="nuget"/></packageSources></configuration>\n')
        archive.writestr('nuget-inputs.json', json.dumps(manifest, indent=2) + '\n')
        archive.writestr('dotnet-inventory.json', json.dumps([
            {'path': path.relative_to(args.dotnet_sdk).as_posix(),
             'size': path.stat().st_size, 'sha256': sha256(path)}
            for path in sdk_files
        ], indent=2) + '\n')
    runtime_manifest = {'version': '4.7.2-ohos.2', 'sha256': sha256(runtime), 'size': runtime.stat().st_size}
    (raw / 'runtime-manifest.json').write_text(json.dumps(runtime_manifest, indent=2) + '\n')
    notices = args.output / 'licenses'
    notices.mkdir()
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
                  'dotnet_host_sha256': sha256(args.dotnet_sdk / 'dotnet'), 'runtime': runtime_manifest}
    provenance['dotnet_buildinfo'] = dotnet_buildinfo
    provenance['native_build'] = native_info
    (args.output / 'build-inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            zip_tree(archive, args.output, args.output.name)
        args.archive.with_suffix(args.archive.suffix + '.sha256').write_text(sha256(args.archive) + '  ' + args.archive.name + '\n')
    print(args.output)


if __name__ == '__main__':
    main()
