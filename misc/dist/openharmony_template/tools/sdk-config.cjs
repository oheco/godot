'use strict';
// Read SDK metadata only. Selection never downloads, modifies, or fabricates SDKs.
const fs = require('node:fs');
const path = require('node:path');
const COMPONENTS = ['native', 'ets', 'js', 'toolchains', 'previewer'];
function requireValue(condition, message) { if (!condition) throw new Error(message); }
function file(filename) { return fs.existsSync(filename) && fs.statSync(filename).isFile(); }
function metadata(filename) {
  requireValue(file(filename), 'Selected SDK component metadata is missing.');
  const value = JSON.parse(fs.readFileSync(filename, 'utf8'));
  requireValue(value && typeof value === 'object' && !Array.isArray(value), 'Invalid SDK metadata document.');
  return value;
}
function api(version) {
  requireValue(typeof version === 'string' && /^[0-9]+\.[0-9]+\.[0-9]+$/.test(version) && Number(version.split('.')[0]) >= 26,
    'Use a HarmonyOS/OpenHarmony SDK version such as 26.0.0 (API 26 or newer).');
  return Number(version.split('.')[0]);
}
function resolveSdk(version, compatibleApi, sdkRoot) {
  const major = api(version);
  requireValue(Number.isInteger(compatibleApi) && compatibleApi >= 26 && compatibleApi <= major,
    'Compatible API must be between 26 and the compile API.');
  requireValue(typeof sdkRoot === 'string' && path.isAbsolute(sdkRoot) && fs.existsSync(sdkRoot) && fs.statSync(sdkRoot).isDirectory(),
    'Selected SDK root must be an existing absolute directory.');
  const root = fs.realpathSync(sdkRoot);
  const bundle = file(path.join(root, 'default/sdk-pkg.json')) ? path.join(root, 'default') : root;
  const descriptor = path.join(bundle, 'sdk-pkg.json');
  const result = {compileSdkVersion:version, targetSdkVersion:version, compatibleSdkVersion:`${compatibleApi}.0.0`, runtimeOS:'OpenHarmony'};
  function components(directory, filename, names = COMPONENTS, identity) {
    for (const component of names) {
      const value = metadata(path.join(directory, component, filename));
      if (!identity) identity = [value.version, value.releaseType];
      requireValue((value.apiVersion === String(major) || value.apiVersion === major) && value.platformVersion === version && value.path === component &&
        typeof value.version === 'string' && value.version && typeof value.releaseType === 'string' && value.releaseType &&
        value.version === identity[0] && value.releaseType === identity[1],
        'Selected SDK components have inconsistent API/version metadata.');
    }
  }
  if (file(descriptor)) {
    requireValue(path.basename(bundle) === 'default', 'HarmonyOS SDK must use a default/sdk-pkg.json bundle.');
    const {data} = metadata(descriptor);
    requireValue(data && (data.apiVersion === String(major) || data.apiVersion === major) && data.platformVersion === version,
      'HarmonyOS SDK bundle does not match the selected API/version.');
    const identity = [data.version, data.releaseType];
    components(path.join(bundle, 'openharmony'), 'oh-uni-package.json', COMPONENTS, identity);
    components(path.join(bundle, 'hms'), 'uni-package.json', ['native', 'ets', 'toolchains'], identity);
    return {...result, runtimeOS:'HarmonyOS', sdkRoot:path.dirname(bundle), openharmonySdkRoot:path.join(bundle, 'openharmony')};
  }
  requireValue(!fs.existsSync(path.join(root, 'hms')) && !fs.existsSync(path.join(root, 'default/hms')),
    'Incomplete HarmonyOS SDK: missing default/sdk-pkg.json.');
  const direct = file(path.join(root, 'native/oh-uni-package.json'));
  requireValue(!direct || path.basename(root) === version,
    'OpenHarmony SDK requires a versioned root: <SDK home>/<full API>/<component>.');
  const foundation = direct ? root : path.join(root, version);
  components(foundation, 'oh-uni-package.json');
  return {...result, sdkRoot:direct ? path.dirname(root) : root, openharmonySdkRoot:foundation};
}
module.exports = {resolveSdk, api};
