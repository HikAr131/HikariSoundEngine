// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
// Run: node --test tools/source-archive-policy.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { isOmittedUpstreamBinary, redistributedDriverFiles } from './source-archive-policy.mjs';

const app = 'upstream/fxsound-app/';
const driver = 'upstream/fxsound-driver-source/';
const extensions = ['exe', 'dll', 'pdb', 'ilk', 'iobj', 'ipdb', 'lib', 'obj', 'exp', 'msi', 'msm', 'msp', 'cab', 'sys',
  'cat', 'syso'];

test('omits every listed binary extension in any case, in the app and the driver source', () => {
  for (const extension of extensions) {
    for (const name of [`tool.${extension}`, `TOOL.${extension.toUpperCase()}`, `Driver - Copy.${extension}`]) {
      assert.equal(isOmittedUpstreamBinary(`${app}bin/x64/${name}`), true, name);
      assert.equal(isOmittedUpstreamBinary(`${driver}fxvad/${name}`), true, name);
    }
  }
});

test('keeps exactly the three redistributed driver files', () => {
  assert.deepEqual(redistributedDriverFiles, ['fxvad.sys', 'fxvad.inf', 'fxvadntamd64.cat']
    .map(name => `${app}Installer/Drivers/Version14/win10/x64/${name}`));
  for (const file of redistributedDriverFiles) assert.equal(isOmittedUpstreamBinary(file), false, file);
  for (const file of ['Version14/win10/arm64/fxvad.sys', 'Version14/win10/x86/fxvadntx86.cat',
    'Version14/win7/x64/fxvadntamd64.cat', 'Version14/win10/x64/fxdevcon64.exe'])
    assert.equal(isOmittedUpstreamBinary(`${app}Installer/Drivers/${file}`), true, file);
});

test('omits .zip only under the app Installer/Drivers directory', () => {
  assert.equal(isOmittedUpstreamBinary(`${app}Installer/Drivers/Version13/Win10Signed/1768513.zip`), true);
  assert.equal(isOmittedUpstreamBinary(`${app}bin/BonusPresets/BonusPresets.zip`), false);
  assert.equal(isOmittedUpstreamBinary(`${driver}Installer/Drivers/package.zip`), false);
});

test('keeps sources, build files, text data, fonts, images and driver metadata', () => {
  for (const file of ['dsp/DfxDsp.vcxproj', 'dsp/DfxDspPrivate.cpp', 'audiopassthru/include/codedefs.h',
    'audiopassthru/audiopassthru.vcxproj', 'bin/x64/Factsoft/1.fac', "bin/BonusPresets/70's.fac", 'LICENSE',
    'fxsound/Fonts/Gilroy-Bold.ttf', 'Installer/Resources/Fonts/NotoSansSC-Bold.otf', 'fxsound/Images/logo-red.png',
    'Installer/Drivers/Version14/win10/arm64/fxvad.inf', 'fxmcp/.mcpbignore', 'release/changelog.txt',
    'Installer/Apps/notes.exe.txt', 'fxsound/Source/exe'])
    assert.equal(isOmittedUpstreamBinary(app + file), false, file);
  for (const file of ['fxvad/fxsound.ico', 'fxvad/pcmex/fxvad.inf', 'fxvad/fxvad.sln'])
    assert.equal(isOmittedUpstreamBinary(driver + file), false, file);
});

test('never omits helper files or paths outside the pinned upstream trees', () => {
  for (const file of ['tools/archive-source.mjs', 'src/main.cpp', 'tools/sample.exe', 'upstream/other/tool.exe',
    'upstream/fxsound-app.exe', 'build-from-source.ps1'])
    assert.equal(isOmittedUpstreamBinary(file), false, file);
});
