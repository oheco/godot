#!/usr/bin/env python3
"""Build an isolated, unsigned generated project with the installed official CLI.

The bundled Node/SDK and a prepared persistent Hvigor CLI cache are required.
Only pre-staged local oh_modules are used; no dependency installation, signing,
installation or device/UI acceptance is performed. The source stays unchanged.
"""
import argparse
from contextlib import contextmanager
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile


DEFAULT_OHECO_ROOT = Path('/storage/Users/currentUser/.oheco')
EXCLUDED = {
    'node_modules', '.hvigor', '.cxx', 'build', '.git', '.bitfun', '.idea',
    '.vscode', '.settings', '.appanalyzer', '.godot-config-history',
    'local.properties', '.npmrc', '.ohpmrc', 'ohpm-config.json5',
    'credentials', 'signing', 'material',
}
CREDENTIAL_PATTERNS = ('*.p12', '*.p7b', '*.cer', '*.pem', '*.key', '*.jks',
                       '*.keystore', '*.mobileprovision', '.env', '.env.*')
# This probe loads only the copied metadata helpers, never installed plugins or
# project task code. resolveToolchain checks the already prepared pnpm cache.
PREFLIGHT = r"""
'use strict';
const fs = require('node:fs');
const path = require('node:path');
try {
  const input = JSON.parse(fs.readFileSync(0, 'utf8'));
  const helper = require(path.join(input.projectDir, 'tools/toolchain-config.cjs'));
  const {resolveSdk} = require(path.join(input.projectDir, 'tools/sdk-config.cjs'));
  const toolchain = helper.resolveToolchain(input.request);
  if (!helper.atLeast(process.versions.node, helper.MIN_NODE_VERSION)) {
    throw new Error('Bundled Node must be version 24.14.1 or newer.');
  }
  const sdk = resolveSdk(input.version, input.compatibleApi, input.request.sdkRoot);
  process.stdout.write(JSON.stringify({toolchain, sdk, nodeVersion:process.versions.node}));
} catch (error) {
  process.stderr.write('Official toolchain preflight: ' + error.message + '\n');
  process.exitCode = 1;
}
"""


def excluded(name):
    lowered = name.lower()
    return lowered in EXCLUDED or any(fnmatch.fnmatchcase(lowered, pattern) for pattern in CREDENTIAL_PATTERNS)


def copy_project(source, destination):
    def ignore(directory, names):
        ignored = [name for name in names if excluded(name)]
        for name in set(names) - set(ignored):
            path = Path(directory) / name
            if path.is_symlink():
                target = path.resolve(strict=True)
                if not target.is_relative_to(source) or any(excluded(part) for part in target.relative_to(source).parts):
                    raise ValueError('Project symlinks must reference retained local project inputs')
        return ignored
    # Materialize local oh_modules links so no build reads back into the source.
    shutil.copytree(source, destination, ignore=ignore, symlinks=False)


def command_line_tools_root(argument):
    if argument:
        root = argument.resolve(strict=True)
    else:
        launcher = (Path(os.environ.get('OHECO_ROOT') or DEFAULT_OHECO_ROOT) / 'bin/hvigorw').resolve(strict=True)
        root = next((parent for parent in launcher.parents
                     if (parent / 'hvigor/bin/hvigorw').is_file() and (parent / 'node/bin/node').is_file()), None)
        if root is None:
            raise ValueError('Cannot derive the official command-line-tools package from OHECO_ROOT/bin/hvigorw')
    if not root.is_dir() or not (root / 'hvigor/bin/hvigorw.js').is_file() or not (root / 'node/bin/node').is_file():
        raise ValueError('Command-line-tools root requires the official hvigor/bin/hvigorw.js and node/bin/node')
    return root


def private(path, mode):
    info = path.lstat()
    if stat.S_IMODE(info.st_mode) != mode or info.st_uid != os.getuid() or path.is_symlink():
        raise ValueError('TMPDIR must enforce caller-owned 0700 directories and 0600 files; hmdfs is unsuitable')


@contextmanager
def private_workspace():
    if not os.environ.get('TMPDIR'):
        raise ValueError('Set TMPDIR to a writable, permission-capable temporary filesystem')
    base = Path(os.environ['TMPDIR']).resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix='godot-hvigor-test-', dir=base)).resolve(strict=True)
    try:
        private(root, 0o700)
        yield root
    finally:
        # Verify the exact mkdtemp target before deleting; never chmod HOME/hmdfs.
        if root.parent != base or root == base or root.resolve() != root or root.is_symlink():
            raise RuntimeError('Refusing to clean an unexpected temporary workspace path')
        shutil.rmtree(root)


def child_environment(root, toolchain_root, hvigor_home, selection):
    env = dict(os.environ)
    for key in list(env):
        if key.upper().startswith(('HVIGOR_', 'OHECO_HVIGOR_')) or key.upper() in {
            'NODE_OPTIONS', 'NODE_PATH', 'NODE_OPTS', 'NODE_HOME', 'JAVA_HOME',
            'DEVECO_NODE_HOME', 'DEVECO_HVIGOR_HOME', 'OHPM_HOME', 'WORKSPACE_DIR',
            'CANGJIE_BUILD_SUPPORT_PATH', 'DEVECO_CANGJIE_PATH', 'DEVECO_HVIGOR_CANGJIE_PLUGIN',
            'DEVECO_SDK_HOME', 'OHOS_SDK_HOME', 'OHOS_BASE_SDK_HOME',
        }:
            env.pop(key)
    for name in ('home', 'tmp', 'config', 'cache'):
        (root / name).mkdir(mode=0o700)
        private(root / name, 0o700)
    env.update(HOME=str(root / 'home'), TMPDIR=str(root / 'tmp'),
               TMP=str(root / 'tmp'), TEMP=str(root / 'tmp'),
               XDG_CONFIG_HOME=str(root / 'config'), XDG_CACHE_HOME=str(root / 'cache'),
               HVIGOR_USER_HOME=str(hvigor_home), NODE_HOME=str(toolchain_root / 'node/bin'),
               npm_config_offline='true', NPM_CONFIG_OFFLINE='true',
               npm_config_cache=str(root / 'cache/npm'), NPM_CONFIG_CACHE=str(root / 'cache/npm'),
               SHELL=shutil.which('sh') or '/usr/bin/sh')
    env['PATH'] = str(toolchain_root / 'node/bin') + os.pathsep + env.get('PATH', '')
    if selection['runtimeOS'] == 'HarmonyOS':
        env['DEVECO_SDK_HOME'] = selection['sdkRoot']
    else:
        env['OHOS_SDK_HOME'] = env['OHOS_BASE_SDK_HOME'] = selection['sdkRoot']
    return env


def stage_offline_inputs(project, read_document, write_document):
    for relative in ('oh-package.json5', 'entry/oh-package.json5'):
        path = project / relative
        document = read_document(path)
        # libentry declarations are already in generated local oh_modules. An
        # empty manifest keeps OHPM out of the validation build entirely.
        for key in ('scripts', 'dynamicDependencies'):
            document.pop(key, None)
        document['dependencies'] = {}
        document['devDependencies'] = {}
        write_document(path, document)
    write_document(project / 'hvigor/hvigor-config.json5', {
        'modelVersion': '6.0.0', 'dependencies': {},
        'execution': {'daemon': False, 'incremental': False, 'parallel': False},
    })
    for relative, task in (('hvigorfile.ts', 'appTasks'), ('entry/hvigorfile.ts', 'hapTasks')):
        (project / relative).write_text(
            f"import {{ {task} }} from '@ohos/hvigor-ohos-plugin';\n"
            f"export default {{ system: {task}, plugins: [] }};\n", encoding='utf-8')
    types = project / 'entry/oh_modules/libentry.so'
    if not (types / 'oh-package.json5').is_file():
        raise ValueError('Generate/stage entry/oh_modules/libentry.so before this offline build')
    declaration = read_document(types / 'oh-package.json5').get('types')
    if not isinstance(declaration, str) or not declaration:
        raise ValueError('Staged libentry.so package must declare its local types file')
    declaration_path = (types / declaration).resolve(strict=True)
    if not declaration_path.is_relative_to(types) or not declaration_path.is_file():
        raise ValueError('Staged libentry.so declarations must be existing local inputs')


def check_wrapper_configuration(project, toolchain, read_document):
    home = Path(toolchain['hvigorUserHome'])
    digest = hashlib.sha256(str(project).encode()).hexdigest()[:32]
    for path, global_config in (
        (home / 'hvigor-config.json5', True),
        (home / 'project_caches' / digest / 'workspace/package.json', False),
    ):
        if not path.exists():
            continue
        document = read_document(path)
        if any(key in document for key in ('scripts', 'dynamicDependencies')) or any(
                document.get(key, {}) != {} for key in ('dependencies', 'devDependencies')):
            raise ValueError('Prepared Hvigor cache must have empty dependencies and no package scripts')
        if global_config:
            if set(document) - {'modelVersion', 'dependencies', 'devDependencies', 'execution'}:
                raise ValueError('Prepared Hvigor cache has unsupported global configuration')
            execution = document.get('execution', {})
            if not isinstance(execution, dict) or set(execution) - {'daemon', 'incremental', 'parallel'} or any(
                    not isinstance(value, bool) for value in execution.values()):
                raise ValueError('Prepared Hvigor cache has invalid execution options')


def build(args, tools, sdk, hvigor_home, output):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    sys.dont_write_bytecode = True
    from project_config import read_document, resolve_sdk, sdk_api, write_document

    source_documents = {}
    metadata = {
        'schemaVersion': 2, 'scope': 'unsigned ArkTS compilation' if args.arkts_only else 'unsigned HAP build',
        'signed': False, 'installed_or_ui_tested': False, 'source_project': str(args.project),
        'source_documents_sha256': {}, 'exit_code': 1, 'artifacts': [],
    }
    log_path = output / 'build.log'
    try:
        for relative in ('build-profile.json5', 'entry/src/main/module.json5'):
            source = args.project / relative
            if not source.resolve(strict=True).is_relative_to(args.project):
                raise ValueError('Generated configuration must reference local source project files')
            source_documents[relative] = source.read_bytes()
        metadata['source_documents_sha256'] = {
            name: hashlib.sha256(data).hexdigest() for name, data in source_documents.items()}
        with private_workspace() as root:
            if any(root.is_relative_to(path) for path in (args.project, tools, sdk, hvigor_home, output)):
                raise ValueError('TMPDIR must isolate temporary work from project/toolchain/cache/output inputs')
            project = root / 'project'
            copy_project(args.project, project)
            profile_path = project / 'build-profile.json5'
            profile = read_document(profile_path)
            app = profile.get('app')
            if not isinstance(app, dict) or not isinstance(app.get('products'), list) or not app['products']:
                raise ValueError('Generated profile requires app.products')
            products = app['products']
            if any(not isinstance(product, dict) for product in products):
                raise ValueError('Generated products must be objects')
            defaults = [product for product in products if product.get('name') == 'default']
            modules = profile.get('modules')
            if (len(defaults) != 1 or not isinstance(modules, list) or len(modules) != 1 or
                    not isinstance(modules[0], dict) or modules[0].get('name') != 'entry' or
                    modules[0].get('srcPath') not in ('entry', './entry')):
                raise ValueError('Expected one default product and the generated entry module')
            selected = defaults[0]
            compatible = selected.get('compatibleSdkVersion', '26.0.0')
            compatible_api = compatible if type(compatible) is int else sdk_api(compatible)
            version = args.openharmony_sdk_version or selected['compileSdkVersion']
            selection = resolve_sdk(version, compatible_api, str(sdk), str(tools))
            app['signingConfigs'] = []
            for product in products:
                product.pop('signingConfig', None)
            for field in ('compileSdkVersion', 'targetSdkVersion', 'compatibleSdkVersion', 'runtimeOS'):
                selected[field] = selection[field]
            write_document(profile_path, profile)
            module_path = project / 'entry/src/main/module.json5'
            module = read_document(module_path)
            original_devices = list(module['module']['deviceTypes'])
            devices = {'default': 'phone'} if selection['runtimeOS'] == 'HarmonyOS' else {'phone': 'default'}
            module['module']['deviceTypes'] = [devices.get(device, device) for device in original_devices]
            write_document(module_path, module)
            stage_offline_inputs(project, read_document, write_document)
            work = root / 'work'
            work.mkdir(mode=0o700)
            private(work, 0o700)
            request = {
                'schemaVersion': 2, 'projectDir': str(project), 'sdkRoot': selection['sdkRoot'],
                'commandLineToolsRoot': str(tools), 'hvigorUserHome': str(hvigor_home),
                'buildMode': 'debug', 'format': 'hap', 'workDir': str(work),
            }
            request_path = root / 'request.json'
            with os.fdopen(os.open(request_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w', encoding='utf-8') as stream:
                json.dump(request, stream)
                stream.write('\n')
            private(request_path, 0o600)
            env = child_environment(root, tools, hvigor_home, selection)
            node = str(tools / 'node/bin/node')
            probe = subprocess.run([node, '--eval', PREFLIGHT], input=json.dumps({
                'projectDir': str(project), 'request': request, 'version': version, 'compatibleApi': compatible_api,
            }), text=True, capture_output=True, cwd=project, env=env, shell=False, timeout=15)
            log_path.write_text(probe.stderr, encoding='utf-8')
            if probe.returncode:
                raise RuntimeError('Official toolchain/cache preflight failed; see build.log')
            preflight = json.loads(probe.stdout)
            toolchain = preflight['toolchain']
            if toolchain['selectedNode'] != node or preflight['sdk'] != selection:
                raise ValueError('Copied helper selection disagrees with the selected Node/SDK metadata')
            check_wrapper_configuration(project, toolchain, read_document)
            metadata.update(sdk=selection, toolchain=toolchain, node_version=preflight['nodeVersion'],
                            build_profile=profile, original_device_types=original_devices,
                            test_device_types=module['module']['deviceTypes'])
            if args.arkts_only:
                command = [toolchain['selectedNode'], '--', toolchain['wrapperJS'], '--mode', 'module',
                           '-p', 'module=entry@default', '-p', 'product=default', '-p', 'buildMode=debug',
                           'default@CompileArkTS', '--no-daemon', '--no-incremental']
            else:
                command = [toolchain['selectedNode'], str(project / 'tools/build.cjs'), '--request', str(request_path)]
            with log_path.open('a', encoding='utf-8') as log:
                result = subprocess.run(command, cwd=project, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, shell=False, timeout=900)
            metadata['exit_code'] = result.returncode
            if result.returncode == 0 and not args.arkts_only:
                runner = read_document(work / 'result.json')
                if runner.get('schemaVersion') != 2 or runner.get('signed') is not False or runner.get('format') != 'hap' or runner.get('mode') != 'debug':
                    raise ValueError('Runner did not report an unsigned schema-2 debug HAP')
                artifact = Path(runner['outputPath'])
                expected = project / 'entry/build/default/outputs/default'
                if artifact.is_symlink() or artifact.resolve(strict=True).parent != expected or not artifact.name.endswith('-unsigned.hap') or artifact.stat().st_size == 0:
                    raise ValueError('Runner returned an unexpected unsigned HAP path')
                target = output / artifact.name
                shutil.copyfile(artifact, target)
                metadata['artifacts'].append(target.name)
                with target.open('rb') as stream:
                    checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
                metadata['artifact_metadata'] = [{'path': str(target), 'size': target.stat().st_size,
                                                 'sha256': checksum}]
                metadata['runner_result'] = {**runner, 'outputPath': str(target)}
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        metadata['exit_code'] = 124 if isinstance(error, subprocess.TimeoutExpired) else 1
        metadata['error'] = str(error)
        with log_path.open('a', encoding='utf-8') as log:
            log.write(f'Validation helper: {error}\n')
    finally:
        try:
            metadata['source_documents_unchanged'] = all(
                (args.project / name).read_bytes() == data for name, data in source_documents.items())
        except OSError:
            metadata['source_documents_unchanged'] = False
        if not metadata['source_documents_unchanged']:
            metadata['exit_code'] = 1
            metadata['error'] = 'Source project configuration changed during the isolated build'
        write_document(output / 'result.json', metadata)
    return metadata['exit_code']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True, help='Generated project with local oh_modules already staged')
    parser.add_argument('--command-line-tools-root', type=Path, help='Official CLI package; default: canonical OHECO_ROOT/bin/hvigorw package')
    parser.add_argument('--sdk', type=Path, help='Prepared SDK home; default: selected CLI package/sdk')
    parser.add_argument('--hvigor-user-home', type=Path, help='Prepared persistent CLI cache; default: HVIGOR_USER_HOME or OHECO_ROOT parent/.hvigor')
    parser.add_argument('--output', type=Path, required=True, help='New persistent log/result/artifact directory outside the source project')
    parser.add_argument('--openharmony-sdk-version', help='Explicit alternate SDK version, e.g. 26.0.0; metadata determines SDK family')
    parser.add_argument('--arkts-only', action='store_true', help='Compile ArkTS with the official wrapper; do not assemble a HAP')
    args = parser.parse_args()
    if sys.platform != 'ohos':
        parser.error('Run with native OpenHarmony Python and the installed official CLI')
    try:
        args.project = args.project.resolve(strict=True)
        if not args.project.is_dir():
            raise ValueError('--project must be a directory')
        tools = command_line_tools_root(args.command_line_tools_root)
        sdk = (args.sdk or tools / 'sdk').resolve(strict=True)
        oheco = Path(os.environ.get('OHECO_ROOT') or DEFAULT_OHECO_ROOT)
        cache = args.hvigor_user_home or Path(os.environ.get('HVIGOR_USER_HOME') or oheco.parent / '.hvigor')
        if not cache.is_absolute():
            raise ValueError('--hvigor-user-home/HVIGOR_USER_HOME must be absolute')
        hvigor_home = cache.resolve(strict=True)
        if not sdk.is_dir() or not hvigor_home.is_dir():
            raise ValueError('SDK and prepared persistent Hvigor cache must be existing directories')
        output = args.output.resolve()
        if any(output.is_relative_to(path) for path in (args.project, tools, sdk, hvigor_home)):
            raise ValueError('--output must be outside the project and installed toolchain/cache inputs')
        output.mkdir(parents=True, exist_ok=False)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    code = build(args, tools, sdk, hvigor_home, output)
    print(output)
    raise SystemExit(code)


if __name__ == '__main__':
    main()
