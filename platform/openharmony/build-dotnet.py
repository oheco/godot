#!/usr/bin/env python3
"""Generate Godot C# bindings and build its managed editor on OpenHarmony offline."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--godot', type=Path, required=True, help='Signed native Godot CLI linked to the editor library')
parser.add_argument('--dotnet-sdk', type=Path, required=True)
parser.add_argument('--nuget-feed', type=Path, required=True)
parser.add_argument('--log', type=Path, required=True)
parser.add_argument('--package-version', default='4.7.2-ohos.2')
args = parser.parse_args()
if sys.platform != 'ohos':
    raise SystemExit('Run this build on the native OpenHarmony host')
source = Path(__file__).resolve().parents[2]
dotnet = args.dotnet_sdk.resolve()
feed = args.nuget_feed.resolve()
# The managed build publishes for `openharmony-arm64`, so it must use the adapted
# OpenHarmony SDK rather than a generic Linux ARM64 one.
for name in ('dotnet', 'sdk', 'host/fxr', 'shared/Microsoft.NETCore.App', 'packs/Microsoft.NETCore.App.Runtime.openharmony-arm64'):
    if not (dotnet / name).exists():
        raise SystemExit(f'Not an adapted OpenHarmony .NET SDK (missing {name}): {dotnet}')
subprocess.run([sys.executable, str(source / 'platform/openharmony/dotnet/prepare-feed.py'), str(feed)], check=True)
with tempfile.TemporaryDirectory(prefix='godot-managed-build-', dir='/data/storage/el2/base/cache') as private:
    env = dict(os.environ)
    env.update(DOTNET_ROOT=str(dotnet), DOTNET_ROOT_ARM64=str(dotnet), DOTNET_CLI_HOME=private,
               TMPDIR=private, DOTNET_OHOS_TMPDIR=private, NUGET_PACKAGES=private + '/nuget',
               DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false',
               DOTNET_CLI_DO_NOT_USE_MSBUILD_SERVER='1', MSBUILDDISABLENODEREUSE='1',
               DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true',
               GODOT_OHOS_DATA_DIR=private + '/godot', GODOT_OHOS_CACHE_DIR=private + '/godot-cache',
               PATH=str(dotnet) + os.pathsep + os.environ.get('PATH', ''))
    Path(env['GODOT_OHOS_DATA_DIR']).mkdir()
    Path(env['GODOT_OHOS_CACHE_DIR']).mkdir()
    commands = [
        [str(args.godot.resolve()), '--headless', '--generate-mono-glue', str(source / 'modules/mono/glue')],
        [sys.executable, str(source / 'modules/mono/build_scripts/build_assemblies.py'),
         '--godot-output-dir', str(source / 'bin'), '--godot-platform', 'openharmony',
         '--package-version', args.package_version,
         '--msbuild-arg=/p:RestoreConfigFile=' + str(feed / 'NuGet.Config'),
         '--msbuild-arg=/p:RestoreLockedMode=true', '--msbuild-arg=/p:NuGetAudit=false',
         '--msbuild-arg=/p:UseSharedCompilation=false', '--msbuild-arg=/m:2'],
    ]
    with args.log.open('w') as log:
        for command in commands:
            log.write('+ ' + ' '.join(command) + '\n'); log.flush()
            subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
print('Native managed editor build completed:', source / 'bin/GodotSharp')
