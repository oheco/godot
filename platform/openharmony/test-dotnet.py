#!/usr/bin/env python3
"""Build/run a real Godot C# scene and test assembly reload in one native editor process."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--godot', type=Path, required=True)
    parser.add_argument('--godotsharp', type=Path, required=True)
    parser.add_argument('--dotnet-sdk', type=Path, required=True)
    parser.add_argument('--nuget-feed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory for logs and the test project')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        raise SystemExit('Run on the native OpenHarmony host')
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    source = Path(__file__).resolve().parent / 'tests/dotnet-smoke'
    project = output / 'CSharp project with spaces'
    for path in source.rglob('*'):
        if '.godot' in path.relative_to(source).parts:
            continue
        if path.is_file():
            target = project / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    godot = str(args.godot.resolve())
    sharp = args.godotsharp.resolve()
    dotnet = args.dotnet_sdk.resolve()
    feed = output / 'feed'
    feed.mkdir()
    for root in (args.nuget_feed, sharp / 'Tools/nupkgs'):
        packages = list(root.glob('*.nupkg'))
        if not packages:
            raise SystemExit(f'Missing offline packages: {root}')
        for package in packages:
            shutil.copyfile(package, feed / package.name)
    with tempfile.TemporaryDirectory(prefix='godot-csharp-acceptance-', dir='/data/storage/el2/base/cache') as private:
        env = dict(os.environ)
        for key in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'GODOT_OHOS_RELOAD_REPORT'):
            env.pop(key, None)
        env.update(DOTNET_ROOT=str(dotnet), DOTNET_ROOT_ARM64=str(dotnet),
                   DOTNET_CLI_HOME=private, DOTNET_OHOS_TMPDIR=private, TMPDIR=private,
                   NUGET_PACKAGES=private + '/nuget', GODOT_NUGET_SOURCE=str(feed),
                   GODOT_SHARP_ROOT=str(sharp), GODOT_OHOS_DATA_DIR=private + '/godot',
                   GODOT_OHOS_CACHE_DIR=private + '/godot-cache',
                   DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false',
                   DOTNET_CLI_DO_NOT_USE_MSBUILD_SERVER='1', MSBUILDDISABLENODEREUSE='1',
                   DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true',
                   PATH=str(dotnet) + os.pathsep + env.get('PATH', ''))
        Path(env['GODOT_OHOS_DATA_DIR']).mkdir()
        Path(env['GODOT_OHOS_CACHE_DIR']).mkdir()
        checks = []

        def run(name, command, marker=None):
            with (output / (name + '.log')).open('w') as log:
                log.write('+ ' + ' '.join(command) + '\n'); log.flush()
                result = subprocess.run(command, cwd=project, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=180)
            if result.returncode or (marker and marker not in (output / (name + '.log')).read_text()):
                raise RuntimeError(f'{name} failed; see {output / (name + ".log")}')
            checks.append(name)

        build = [str(dotnet / 'dotnet'), 'build', 'Smoke.csproj', '-c', 'Debug', '--nologo',
                 '/p:NuGetAudit=false', '/p:UseSharedCompilation=false', '/m:2']
        run('scene-build', build)
        run('editor-import', [godot, '--headless', '--editor', '--path', str(project), '--quit'])
        run('scene-run', [godot, '--headless', '--path', str(project), '--', '--verify'], 'OHOS_CSHARP_SMOKE_PASS')

        report = Path(private) / 'reload-result.txt'
        env['GODOT_OHOS_RELOAD_REPORT'] = str(report)
        with (output / 'editor-reload.log').open('w') as log:
            command = [godot, '--headless', '--editor', '--path', str(project)]
            log.write('+ ' + ' '.join(command) + '\n'); log.flush()
            editor = subprocess.Popen(command, cwd=project, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 120
                while not report.exists() or report.read_text() != '100':
                    if editor.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Editor did not load the original C# assembly; see editor-reload.log')
                    time.sleep(0.2)
                marker = project / 'ReloadMarker.cs'
                marker.write_text(marker.read_text().replace('=> 100;', '=> 200;'))
                run('scene-rebuild', build)
                if editor.wait(timeout=120) != 0 or report.read_text() != '200':
                    raise RuntimeError('Editor did not observe the rebuilt assembly')
                checks.append('same-process-hot-reload')
            finally:
                if editor.poll() is None:
                    editor.terminate()
                    try:
                        editor.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        editor.kill()
                        editor.wait()
    (output / 'result.json').write_text(json.dumps({
        'platform': sys.platform, 'passed': checks,
        'scope': 'Native terminal: not a HAP sandbox or Vulkan presentation test',
    }, indent=2) + '\n')
    print('Native Godot C# acceptance passed:', output)


if __name__ == '__main__':
    main()
