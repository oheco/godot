#!/usr/bin/env python3
"""Package the optional oo Godot development kit; never install or publish it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import zipfile

from development_kit import DOTNET_PACKAGE_VERSION, copy_managed, managed_inputs, requirements
from project_config import HOST_ABI_VERSION, read_document


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True, help='Signed launcher directory, including libgodot.so and build-info.json')
    parser.add_argument('--godotsharp', type=Path, required=True, help='Built managed API and editor tools')
    parser.add_argument('--dotnet-sdk', type=Path, required=True, help='External build SDK, validated but never copied')
    parser.add_argument('--debug-template', type=Path, required=True)
    parser.add_argument('--release-template', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New package directory')
    parser.add_argument('--archive', type=Path, help='Optional new tar.gz; strip one directory when installing')
    parser.add_argument('--version', default='4.7.2-ohos.7-dev')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    if args.output.exists() or (args.archive and args.archive.exists()):
        parser.error('Use new output and archive paths; existing packages are preserved')
    packages, dotnet_info = managed_inputs(args.godotsharp.resolve(), args.dotnet_sdk.resolve())
    native = read_document(args.cli / 'build-info.json')
    library = args.cli / 'libgodot.so'
    if native.get('openharmony_host_abi') != HOST_ABI_VERSION or native.get('libgodot.so_sha256') != sha256(library):
        parser.error('Headless library must match its shared-host ABI provenance')
    for name in ('godot', 'libgodot.so'):
        path = args.cli / name
        if native.get(name + '_sha256') != sha256(path):
            parser.error(f'Native CLI input digest mismatch: {name}')
        with path.open('rb') as stream:
            header = stream.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
            parser.error(f'Expected final signed ELF64 ARM64 input: {path}')
    templates = {'openharmony_debug_arm64-v8a.zip': args.debug_template,
                 'openharmony_release_arm64-v8a.zip': args.release_template}
    for path in templates.values():
        with zipfile.ZipFile(path) as archive:
            metadata = json.loads(archive.read('entry/src/main/resources/rawfile/godot_template.json'))
            if metadata.get('hostAbi') != HOST_ABI_VERSION:
                parser.error(f'Incompatible game template ABI: {path}')
            if 'tools/auto-sign' not in archive.namelist() or 'tools/auto-sign.cjs' in archive.namelist():
                parser.error(f'Expected the current native auto-signing template: {path}')
    hashes = {name: sha256(path) for name, path in templates.items()}
    template_set = hashlib.sha256(json.dumps(hashes, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    root = args.output.resolve()
    root.mkdir(parents=True)
    (root / 'bin').mkdir()
    for name in ('godot', 'libgodot.so'):
        shutil.copy2(args.cli / name, root / 'bin' / name)
    shutil.copy2(args.cli / 'build-info.json', root / 'bin/build-info.json')
    managed = root / 'share/godot'
    copy_managed(args.godotsharp.resolve(), managed / 'GodotSharp', packages)
    target = managed / 'templates'
    target.mkdir()
    for name, path in templates.items():
        shutil.copy2(path, target / name)
    (target / '.ready').write_text(template_set + '\n')
    manifest = {
        **requirements(sha256(library)),
        'packageName': 'godot-editor',
        'packageVersion': args.version,
        'headless': 'bin/godot',
        'managedRoot': 'share/godot',
        'templatesRoot': 'share/godot/templates',
        'templateSetSha256': template_set,
        'templateSha256': hashes,
        'godotPackages': [path.name for path in packages],
        'dependencies': [{'name': 'dotnet-sdk', 'constraint': '=' + DOTNET_PACKAGE_VERSION,
                          'version_basis': 'package', 'platforms': ['ohos-arm64']}],
        'thirdPartyRestore': 'normal NuGet sources; no bundled offline feed',
        'dotnetBuildInfo': dotnet_info,
        'nativeBuild': native,
    }
    (root / 'godot-devkit.json').write_text(json.dumps(manifest, indent=2) + '\n')
    notices = root / 'licenses'
    notices.mkdir()
    for name in ('LICENSE.txt', 'COPYRIGHT.txt', 'AUTHORS.md'):
        shutil.copy2(source / name, notices / name)
    vulkan = source / 'thirdparty/vulkan'
    for item in read_document(vulkan / 'openharmony-inputs.json')['archives']:
        path = Path(item['license_file'])
        dest = notices / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / path, dest)
    (root / 'README.md').write_text(
        '# Godot OpenHarmony development kit\n\n'
        'Optional companion to the thin Editor app. Includes the signed native headless '
        'launcher and its shared engine, managed editor/API components, four engine-matched '
        'Godot NuGet packages and both export templates. Requires oo dotnet-sdk '
        + DOTNET_PACKAGE_VERSION + '. Third-party game dependencies use normal dotnet restore.\n\n'
        'Install layout: OHECO_ROOT/packages/godot-editor/<version>; the active command is '
        'bin/godot. The app discovers compatible kits automatically. '
        'GODOT_OHOS_DEVKIT_ROOT selects a kit explicitly. '
        'No complete .NET SDK or general offline NuGet feed is included.\n')
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.archive, 'w:gz') as archive:
            archive.add(root, arcname=root.name)
        args.archive.with_suffix(args.archive.suffix + '.sha256').write_text(sha256(args.archive) + '  ' + args.archive.name + '\n')
    print(json.dumps({'output': str(root), 'manifest': manifest,
                      'archive': str(args.archive) if args.archive else None,
                      'archiveSha256': sha256(args.archive) if args.archive else None}, indent=2))


if __name__ == '__main__':
    main()
