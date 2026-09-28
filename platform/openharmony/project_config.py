"""Configuration and staging for the single OpenHarmony DevEco host project.

Inputs and newly generated .json5 documents use the JSON subset of JSON5. When
updating DevEco-owned profiles, comments and trailing commas are accepted too.
Signing fields are preserved; unsupported JSON5 syntax fails rather than losing
user configuration. This module has no third-party or network dependencies.
"""
import copy
import json
import re
import shutil
import os
import tempfile
import uuid
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
TEMPLATE = SOURCE / 'misc/dist/openharmony_template'
HOST_PATH = 'entry/src/main/resources/rawfile/godot_host.json'
SDK_VERSION = '6.1.0(23)'
HOST_ABI_VERSION = 1
HEADERS = ('bridge_openharmony.h', 'engine_host_openharmony.h')
EXCLUDED = {'.hvigor', '.cxx', 'node_modules', 'oh_modules', 'build', '.git', '.idea', '.appanalyzer', '.bitfun', '.godot-config-history'}
RESTRICTED = {
    'ohos.permission.kernel.LOAD_INDEPENDENT_LIBRARY',
    'ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY',
    'ohos.permission.CUSTOM_SANDBOX',
}


def read_document(path):
    """Read JSON with string-aware removal of JSON5 comments/trailing commas."""
    text = Path(path).read_text(encoding='utf-8')
    # Consume strings before comments: URLs, escaped quotes and comment-like
    # signing passwords must never be rewritten.
    token = re.compile(r'"(?:[^"\\]|\\.)*"|//[^\r\n]*|/\*[\s\S]*?\*/')
    text = token.sub(lambda m: m[0] if m[0].startswith('"') else ' ', text)
    token = re.compile(r'"(?:[^"\\]|\\.)*"|,\s*(?=[}\]])')
    text = token.sub(lambda m: m[0] if m[0].startswith('"') else '', text)
    try:
        result = json.loads(text)
    except json.JSONDecodeError as error:
        raise ValueError(f'{path}: unsupported or invalid JSON5; preserve the original and convert it to JSON with optional comments/trailing commas') from error
    if not isinstance(result, dict):
        raise ValueError(f'{path}: expected an object')
    return result


def write_document(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def write_project_documents(project, documents, keep_history=True):
    """Prepare all bytes first; never truncate the only copy of signing settings.

    Temporary staging stays in TMPDIR. On a different filesystem (HOME is often
    hmdfs), protected profiles are renamed to durable configuration history before
    installation. A failed copy restores them with same-filesystem rename, which
    needs no file-content writes even under ENOSPC/RLIMIT_FSIZE. Successful history
    is retained for manual/crash recovery, and excluded from distributed archives.
    """
    protected = {'build-profile.json5', 'entry/build-profile.json5'} if keep_history else set()
    changed = {}
    for name, document in documents.items():
        target = project / name
        if target.is_file() and read_document(target) == document:
            continue  # Preserve original bytes, comments and signing formatting.
        changed[name] = document
    if not changed:
        return
    with tempfile.TemporaryDirectory(prefix='godot-project-config-', dir=os.environ.get('TMPDIR')) as temporary:
        staging = Path(temporary)
        for index, (name, document) in enumerate(changed.items()):
            path = staging / str(index)
            with path.open('w', encoding='utf-8') as stream:
                stream.write(json.dumps(document, indent=2, ensure_ascii=False) + '\n')
                stream.flush()
                os.fsync(stream.fileno())
        history = project / '.godot-config-history' / uuid.uuid4().hex
        backups = {}
        try:
            # Protect all original profiles before installing any document.
            for name in changed.keys() & protected:
                target = project / name
                if target.is_file():
                    backup = history / name
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    os.replace(target, backup)
                    backups[name] = backup
            for index, name in enumerate(changed):
                target = project / name
                target.parent.mkdir(parents=True, exist_ok=True)
                staged = staging / str(index)
                if staged.stat().st_dev == target.parent.stat().st_dev:
                    os.replace(staged, target)
                else:
                    shutil.copyfile(staged, target)
        except BaseException:
            for name, backup in backups.items():
                os.replace(backup, project / name)
            raise


def merge_known(base, overrides, prefix=''):
    for key, value in overrides.items():
        name = prefix + key
        if key not in base:
            raise ValueError(f'Unknown configuration field: {name}')
        if isinstance(base[key], dict):
            if not isinstance(value, dict):
                raise ValueError(f'{name}: expected an object')
            merge_known(base[key], value, name + '.')
        else:
            if type(value) is not type(base[key]):
                raise ValueError(f'{name}: invalid value type')
            base[key] = copy.deepcopy(value)
    return base


def validate_sdk(version):
    match = re.fullmatch(r'\d+\.\d+\.\d+\((\d+)\)', version)
    if not match or int(match[1]) < 23:
        raise ValueError('The shared host requires API 23 or newer (default 6.1.0(23))')


def configuration(profile='game', overrides=None):
    config = read_document(TEMPLATE / HOST_PATH)
    config.update({
        'application': {'bundleId': 'org.godotengine.template', 'displayName': 'template',
                        'vendor': 'example', 'versionCode': 1000000, 'versionName': '1.0.0',
                        'deviceTypes': ['phone', 'tablet', '2in1'], 'orientation': 'portrait',
                        'icons': {'foreground': '', 'background': ''}},
        'build': {'sdkVersion': SDK_VERSION, 'architectures': ['arm64-v8a']},
        'engine': {'target': 'template_release'},
        'permissions': [],
    })
    if profile == 'editor':
        merge_known(config, read_document(Path(__file__).parent / 'profiles/editor.json'))
    elif profile != 'game':
        raise ValueError(f'Unknown host profile: {profile}')
    if overrides:
        merge_known(config, overrides)
    validate_configuration(config)
    return config


def validate_configuration(config):
    if config['schemaVersion'] != 1:
        raise ValueError('Unsupported host schemaVersion')
    role = config['host']['role']
    launch, instances, managed = config['launch'], config['instances'], config['managed']
    if role not in ('game', 'editor'):
        raise ValueError('host.role must be game or editor')
    expected = 'packaged-game' if role == 'game' else 'project-manager'
    if launch['defaultMode'] != expected:
        raise ValueError(f'{role} requires launch.defaultMode={expected}')
    if not all(isinstance(arg, str) and '\0' not in arg for arg in launch['defaultArguments']):
        raise ValueError('launch.defaultArguments must contain strings without NUL')
    if instances['policy'] not in ('disabled', 'editor') or not 1 <= instances['maxCount'] <= 5:
        raise ValueError('Invalid instance policy/count (supported count: 1..5)')
    if managed['mode'] not in ('none', 'sdk') or managed['sdkSource'] != 'oheco':
        raise ValueError('Supported managed modes are none/sdk with sdkSource=oheco; runtime-only packaging is not implemented')
    if role == 'game' and (instances['policy'] != 'disabled' or managed['mode'] != 'none' or launch['acceptProjectRequests']):
        raise ValueError('Packaged games cannot enable editor instances, SDK or project requests')
    if config['diagnostics']['level'] not in ('normal', 'verbose'):
        raise ValueError('Invalid diagnostics.level')
    app = config['application']
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+', app['bundleId']):
        raise ValueError('Invalid application.bundleId')
    if not app['displayName'] or not app['versionName'] or not 1 <= app['versionCode'] <= 2147483647:
        raise ValueError('Application name/version must be non-empty and versionCode within 1..2147483647')
    orientations = {'system', 'landscape', 'landscape_inverted', 'auto_rotation_landscape',
                    'auto_rotation_landscape_restricted', 'portrait', 'portrait_inverted',
                    'auto_rotation_portrait', 'auto_rotation_portrait_restricted',
                    'auto_rotation_unspecified', 'auto_rotation_restricted', 'follow_recent', 'follow_desktop'}
    if app['orientation'] not in orientations:
        raise ValueError('Invalid application.orientation')
    for icon in app['icons'].values():
        if icon and (not Path(icon).is_file() or Path(icon).read_bytes()[:8] != b'\x89PNG\r\n\x1a\n'):
            raise ValueError('Custom application icons must be existing PNG files')
    targets = ('editor',) if role == 'editor' else ('template_debug', 'template_release')
    if config['engine']['target'] not in targets:
        raise ValueError(f'{role} requires engine.target in {targets}')
    if not app['deviceTypes'] or any(t not in ('phone', 'tablet', '2in1') for t in app['deviceTypes']):
        raise ValueError('Invalid application.deviceTypes')
    if role == 'editor' and app['deviceTypes'] != ['2in1']:
        raise ValueError('The editor instance/SDK profile is supported on 2in1 only')
    if not config['build']['architectures'] or any(a not in ('arm64-v8a', 'x86_64') for a in config['build']['architectures']):
        raise ValueError('Unsupported build.architectures')
    validate_sdk(config['build']['sdkVersion'])
    for permission in config['permissions']:
        if not isinstance(permission, str) or not permission.startswith('ohos.permission.'):
            raise ValueError('permissions must contain OpenHarmony permission names')
        if permission in RESTRICTED:
            raise ValueError('Restricted kernel/sandbox permissions require the explicit generator ACL flags')
        if permission not in {'ohos.permission.INTERNET', 'ohos.permission.MICROPHONE',
                              'ohos.permission.LOCK_WINDOW_CURSOR', 'ohos.permission.READ_WRITE_USER_FILE',
                              'ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE'}:
            raise ValueError(f'Unsupported permission metadata: {permission}')
        if permission in {'ohos.permission.READ_WRITE_USER_FILE', 'ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE'} and app['deviceTypes'] != ['2in1']:
            raise ValueError(f'{permission} requires a 2in1-only application profile')


def stage_template(destination):
    """Copy only reproducible source inputs, never IDE caches or stale binaries."""
    destination = Path(destination)
    for source in sorted(TEMPLATE.rglob('*')):
        relative = source.relative_to(TEMPLATE)
        if any(part in EXCLUDED for part in relative.parts):
            continue
        if source.is_symlink():
            raise ValueError(f'Template symlinks are not supported: {relative}')
        name = relative.as_posix()
        if not source.is_file() or name in ('.gitignore', 'local.properties'):
            continue
        if name.startswith(('entry/libs/', 'entry/src/main/cpp/libs/', 'entry/src/main/cpp/include/')):
            continue
        if name.startswith('entry/src/main/resources/rawfile/') and name != HOST_PATH:
            continue
        if source.suffix in ('.p12', '.p7b', '.cer', '.so', '.zip'):
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def set_string(document, name, value):
    for entry in document['string']:
        if entry['name'] == name:
            entry['value'] = value
            return
    document['string'].append({'name': name, 'value': value})


def configure_project(project, config, restricted=(), *, dry_run=False, preserved_profiles=None, keep_history=True):
    """Apply profile fields, preserving signing and unrelated build options.

    Parse everything before writing anything, including existing DevEco-owned
    profiles, so an unsupported profile cannot be silently clobbered.
    """
    validate_configuration(config)
    if not set(restricted).issubset(RESTRICTED):
        raise ValueError('Unknown restricted permission')
    project = Path(project)
    names = ('AppScope/app.json5', 'build-profile.json5', 'entry/build-profile.json5',
             'entry/src/main/module.json5', 'AppScope/resources/base/element/string.json',
             'entry/src/main/resources/base/element/string.json', 'oh-package.json5')
    documents = {name: read_document(project / name) for name in names}
    if preserved_profiles is not None:
        for name in ('build-profile.json5', 'entry/build-profile.json5'):
            path = Path(preserved_profiles) / name
            if path.is_file():
                documents[name] = read_document(path)
            elif list((Path(preserved_profiles) / '.godot-config-history').glob('*/' + name)):
                raise ValueError(f'{path} is missing after a previous migration; restore its durable .godot-config-history backup before updating')
    # Reject structural errors too, before the generator copies/deletes sources.
    root_build = documents['build-profile.json5'].get('app')
    if not isinstance(root_build, dict) or not isinstance(root_build.get('products'), list) or not root_build['products']:
        raise ValueError('build-profile.json5 requires app.products as a nonempty array')
    if any(not isinstance(product, dict) for product in root_build['products']):
        raise ValueError('build-profile.json5 app.products entries must be objects')
    entry_profile = documents['entry/build-profile.json5']
    if not isinstance(entry_profile.get('buildOption', {}), dict) or not isinstance(entry_profile.get('buildOptionSet', []), list):
        raise ValueError('entry/build-profile.json5 has invalid build options')
    for option in [entry_profile.get('buildOption', {}), *entry_profile.get('buildOptionSet', [])]:
        if not isinstance(option, dict) or not isinstance(option.get('nativeLib', {}), dict):
            raise ValueError('entry/build-profile.json5 native/build options must be objects')
        if not isinstance(option.get('nativeLib', {}).get('debugSymbol', {}), dict):
            raise ValueError('entry/build-profile.json5 nativeLib.debugSymbol must be an object')
    if not isinstance(entry_profile.get('buildOption', {}).get('externalNativeOptions', {}), dict):
        raise ValueError('entry/build-profile.json5 externalNativeOptions must be an object')
    app_input, role = config['application'], config['host']['role']
    app = documents['AppScope/app.json5']['app']
    for target, source in (('bundleName', 'bundleId'), ('vendor', 'vendor'),
                           ('versionCode', 'versionCode'), ('versionName', 'versionName')):
        app[target] = app_input[source]
    if config['instances']['policy'] == 'editor':
        app['multiAppMode'] = {'multiAppModeType': 'multiInstance', 'maxCount': config['instances']['maxCount']}
    else:
        app.pop('multiAppMode', None)
    for product in documents['build-profile.json5']['app']['products']:
        for field in ('compileSdkVersion', 'targetSdkVersion', 'compatibleSdkVersion'):
            product[field] = config['build']['sdkVersion']
        product['runtimeOS'] = 'HarmonyOS'
    entry = documents['entry/build-profile.json5']
    build = entry.setdefault('buildOption', {})
    native = build.setdefault('externalNativeOptions', {})
    native['path'] = './src/main/cpp/CMakeLists.txt'
    native['abiFilters'] = config['build']['architectures']
    for option in [build, *entry.get('buildOptionSet', [])]:
        option.setdefault('nativeLib', {}).setdefault('debugSymbol', {})['strip'] = False
    module = documents['entry/src/main/module.json5']['module']
    module['mainElement'] = 'EntryAbility'
    module['deviceTypes'] = app_input['deviceTypes']
    ability = module['abilities'][0]
    ability['name'] = 'EntryAbility'
    ability['srcEntry'] = './ets/entryability/EntryAbility.ets'
    if config['instances']['policy'] == 'editor':
        ability['launchType'] = 'multiton'
    else:
        ability.pop('launchType', None)
    if app_input['orientation'] == 'system':
        ability.pop('orientation', None)
    else:
        ability['orientation'] = app_input['orientation']
    if role == 'editor':
        module.pop('extensionAbilities', None)
    permissions = set(config['permissions'])
    if role == 'editor':
        permissions.update(('ohos.permission.INTERNET', 'ohos.permission.LOCK_WINDOW_CURSOR'))
    if config['managed']['mode'] == 'sdk':
        permissions.update(('ohos.permission.READ_WRITE_USER_FILE', 'ohos.permission.ALLOW_EXTERNAL_NATIVE_CODE'))
    permissions.update(restricted)
    user_reasons = {'ohos.permission.MICROPHONE': 'MICROPHONE_reason',
                    'ohos.permission.READ_WRITE_USER_FILE': 'reason_user_file'}
    module['requestPermissions'] = []
    for name in sorted(permissions):
        item = {'name': name}
        if name in user_reasons:
            item.update(reason='$string:' + user_reasons[name],
                        usedScene={'abilities': ['EntryAbility'], 'when': 'inuse'})
        module['requestPermissions'].append(item)
    strings = documents['entry/src/main/resources/base/element/string.json']
    set_string(strings, 'EntryAbility_label', app_input['displayName'])
    set_string(strings, 'EntryAbility_desc', app_input['displayName'])
    set_string(strings, 'user_permissions', ','.join(sorted(permissions.intersection(user_reasons))))
    set_string(documents['AppScope/resources/base/element/string.json'], 'app_name', app_input['displayName'])
    documents['oh-package.json5']['name'] = 'godot-editor' if role == 'editor' else 'godot-host'
    runtime = {key: copy.deepcopy(value) for key, value in config.items()
               if key not in ('application', 'build', 'engine', 'permissions')}
    if dry_run:
        return
    documents[HOST_PATH] = runtime
    write_project_documents(project, documents, keep_history=keep_history)
    for name, source in app_input['icons'].items():
        if source:
            for directory in ('AppScope/resources/base/media', 'entry/src/main/resources/base/media'):
                shutil.copyfile(source, project / directory / (name + '.png'))
