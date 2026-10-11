"""Synthetic SDK metadata only; these fixtures contain no executable tools."""
from pathlib import Path
from project_config import SDK_COMPONENTS, write_document


def make_sdk(root, family='OpenHarmony', version='26.0.0'):
    root = Path(root)
    data = {'apiVersion': version.split('.')[0], 'platformVersion': version,
            'version': version + '.105', 'releaseType': 'Release', 'stage': 'Release'}
    foundation = root / 'default/openharmony' if family == 'HarmonyOS' else root / version
    for component in SDK_COMPONENTS:
        write_document(foundation / component / 'oh-uni-package.json', dict(data, path=component))
    if family == 'HarmonyOS':
        write_document(root / 'default/sdk-pkg.json', {'meta': {'version': '1.0.0'}, 'data': dict(data, path='HarmonyOS-' + version)})
        for component in ('native', 'ets', 'toolchains'):
            write_document(root / 'default/hms' / component / 'uni-package.json', dict(data, path=component))
    return root
