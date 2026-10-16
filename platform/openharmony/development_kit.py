"""Shared metadata and managed inputs for the optional OpenHarmony development kit."""
import shutil
from pathlib import Path

from project_config import HOST_ABI_VERSION, read_document

ENGINE_VERSION = '4.7.2'
MANAGED_VERSION = '4.7.2-ohos.3'
DOTNET_PACKAGE_VERSION = '10.0.401-ohos.2'
GODOT_PACKAGES = ('Godot.NET.Sdk', 'Godot.SourceGenerators', 'GodotSharp', 'GodotSharpEditor')


def requirements(library_sha256):
    return {
        'schemaVersion': 1,
        'engineVersion': ENGINE_VERSION,
        'hostAbiVersion': HOST_ABI_VERSION,
        'managedVersion': MANAGED_VERSION,
        'architecture': 'arm64-v8a',
        'engineLibrarySha256': library_sha256,
    }


def managed_inputs(godotsharp, dotnet_sdk):
    required = [godotsharp / name for name in (
        'Api/Debug/GodotSharp.dll', 'Api/Release/GodotSharp.dll', 'Tools/GodotTools.dll')]
    packages = [godotsharp / 'Tools/nupkgs' / f'{name}.{MANAGED_VERSION}.nupkg' for name in GODOT_PACKAGES]
    required.extend(packages)
    required.extend(dotnet_sdk / name for name in ('dotnet', 'BUILDINFO.json'))
    for path in required:
        if not path.is_file():
            raise ValueError(f'Missing development-kit input: {path}')
    for name in ('sdk', 'host/fxr', 'shared/Microsoft.NETCore.App',
                 'packs/Microsoft.NETCore.App.Ref', 'packs/Microsoft.NETCore.App.Runtime.openharmony-arm64'):
        if not (dotnet_sdk / name).is_dir():
            raise ValueError(f'Missing external OpenHarmony .NET SDK component: {name}')
    buildinfo = read_document(dotnet_sdk / 'BUILDINFO.json')
    if buildinfo.get('rid') != 'openharmony-arm64':
        raise ValueError('Expected an OpenHarmony ARM64 .NET SDK')
    return packages, buildinfo


def copy_managed(godotsharp, destination, packages):
    """Keep runtime DLLs and exactly one copy of the four Godot build packages."""
    shutil.copytree(godotsharp / 'Api', destination / 'Api')
    tools = godotsharp / 'Tools'
    shutil.copytree(tools, destination / 'Tools',
                    ignore=lambda directory, names: ['nupkgs'] if Path(directory) == tools else [])
    target = destination / 'Tools/nupkgs'
    target.mkdir()
    for package in packages:
        shutil.copy2(package, target / package.name)
