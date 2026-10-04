// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
const root = path.resolve(import.meta.dirname, '..');
const check = () => spawnSync(process.execPath, ['tools/verify-upstream.mjs'], { cwd: root, encoding: 'utf8' });
if (check().status !== 0) throw new Error('Baseline upstream verification failed');
const patch = fs.readFileSync(path.join(root, 'patches/01-headless-host.patch'), 'utf8');
let cases = 0;
function mutate(file, transform) {
  const original = fs.readFileSync(file);
  try {
    const changed = transform(original.toString('utf8'));
    if (changed === original.toString('utf8')) throw new Error('Mutation did not change its target');
    fs.writeFileSync(file, changed);
    const result = check();
    if (result.status === 0 || !result.stderr.includes('Error:')) throw new Error('Mutation was not rejected');
    ++cases;
  } finally { fs.writeFileSync(file, original); fs.utimesSync(file, new Date(), new Date()); }
}
for (const match of patch.matchAll(/^\+\+\+ b\/(.+)$/gm)) mutate(path.join(root, 'build/upstream', match[1]), value => value.replace('// Hikari modification 2026-10-04:', '// Removed modification notice:'));
mutate(path.join(root, 'build/upstream/audiopassthru/src/AudioPassthru/AudioPassthruPrivate.cpp'), value => value.replace('hikariProcessAudio(p_dfx_dsp_', 'removedProcessAudio(p_dfx_dsp_'));
mutate(path.join(root, 'build/upstream/audiopassthru/src/sndDevices/sndDevicesSet.cpp'), value => value.replace('if (!hikariAllowDefaultSwitch(cast_handle->pwszID[device_index_num])) return OKAY;', 'if (false) return OKAY;'));
const setupPath = path.join(root, 'build/upstream/audiopassthru/src/sndDevices/sndDevicesSetupDevices.cpp');
for (let site = 0; site < 4; ++site) mutate(setupPath, value => {
  let index = 0;
  return value.replace(/hikariValidateAudioInitialization\((pwfx|pClosestMatch), &cast_handle->wfx(Capture|Playback)\)/g,
    match => index++ === site ? 'true' : match);
});
const formatPath = path.join(root, 'src/audio_format.cpp');
for (const mask of ['0x3', '0x33', '0x3f', '0x60f', '0x63f']) mutate(formatPath,
  value => value.replace(new RegExp(`mask == ${mask}(?![0-9a-f])`), 'mask == 0'));
for (const field of ['wFormatTag', 'nChannels', 'nSamplesPerSec', 'nAvgBytesPerSec', 'nBlockAlign', 'wBitsPerSample', 'cbSize'])
  mutate(formatPath, value => value.replace(`format->${field} == cached.${field}`, 'true'));
mutate(formatPath, value => value.replace('format->nChannels == 2 && format->cbSize == 0', 'format->nChannels == 4 && format->cbSize == 0'));
if (check().status !== 0) throw new Error('Restored source verification failed');
console.log(`Upstream verification mutation checks passed (${cases} rejected, original restored).`);
