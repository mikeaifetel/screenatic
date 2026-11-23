const path = require('path');
const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');

// The probe's TurboModule lives in ../native and is linked as `file:../native`
// (node_modules/@amazon-devices/probenative is a symlink). Metro only bundles files under its
// watch folders, so without this the app fails at runtime with
// "Cannot find module '@amazon-devices/probenative'".
const nativeDir = path.resolve(__dirname, '../native');

/**
 * Metro configuration
 * https://facebook.github.io/metro/docs/configuration
 *
 * @type {import('metro-config').MetroConfig}
 */
const config = {
  watchFolders: [nativeDir],
  resolver: {
    // Resolve the module's own imports (keplerscript-turbomodule-api, react-native) from the
    // app's node_modules, so there is a single copy of each in the bundle.
    nodeModulesPaths: [path.resolve(__dirname, 'node_modules')],
    blockList: [new RegExp(`${nativeDir.replace(/[/\\]/g, '[/\\\\]')}[/\\\\]node_modules[/\\\\].*`)],
  },
};

module.exports = mergeConfig(getDefaultConfig(__dirname), config);
