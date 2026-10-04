// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'..');
const patch=fs.readFileSync(path.join(root,'patches/01-headless-host.patch'),'utf8');
for(const match of patch.matchAll(/^\+\+\+ b\/(.+)$/gm)) {
  const file=path.join(root,'build/upstream',match[1]);
  if(!fs.readFileSync(file,'utf8').startsWith('// Hikari modification 2026-10-04:')) throw new Error(`Patch was not applied: ${file}`);
}
const audio=fs.readFileSync(path.join(root,'build/upstream/audiopassthru/src/AudioPassthru/AudioPassthruPrivate.cpp'),'utf8');
if(!audio.includes('hikariProcessAudio(p_dfx_dsp_') || /p_dfx_dsp_->(?:setSignalFormat|processAudio)\(/.test(audio)) throw new Error('DSP calls must be serialized by host');
const device=fs.readFileSync(path.join(root,'build/upstream/audiopassthru/src/sndDevices/sndDevicesSet.cpp'),'utf8');
if(!device.includes('if (!hikariAllowDefaultSwitch(cast_handle->pwszID[device_index_num])) return OKAY;')) throw new Error('Default-device host gate absent');
const stripComments = value => value.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '');
const setup = stripComments(fs.readFileSync(path.join(root, 'build/upstream/audiopassthru/src/sndDevices/sndDevicesSetupDevices.cpp'), 'utf8'));
const initializations = [...setup.matchAll(/cast_handle->pAudioClient(Capture|Playback)->Initialize\(/g)];
const guarded = [...setup.matchAll(/hr\s*=\s*hikariValidateAudioInitialization\((pwfx|pClosestMatch),\s*&cast_handle->wfx(Capture|Playback)\)\s*\?\s*cast_handle->pAudioClient(Capture|Playback)->Initialize\(([\s\S]*?)\)\s*:\s*AUDCLNT_E_UNSUPPORTED_FORMAT\s*;/g)];
if (initializations.length !== 4 || guarded.length !== initializations.length ||
    guarded.filter(match => match[2] === 'Capture').length !== 1 ||
    guarded.filter(match => match[1] === 'pClosestMatch').length !== 2 ||
    guarded.some(match => match[2] !== match[3] ||
      !new RegExp(`,\\s*${match[1]},\\s*NULL\\s*$`).test(match[4])))
  throw new Error('Every WASAPI Initialize call must validate its actual format against the corresponding cached format');
const format = stripComments(fs.readFileSync(path.join(root, 'src/audio_format.cpp'), 'utf8'));
for (const [channels, masks] of [[2, ['0x3']], [4, ['0x33']], [6, ['0x3f', '0x60f']], [8, ['0x63f']]]) {
  const expected = `case ${channels}: return ${masks.map(mask => `mask == ${mask}`).join(' || ')};`;
  if (!format.includes(expected)) throw new Error(`Unsupported or ambiguous ${channels}-channel layout accepted`);
}
if (!format.includes('default: return false;') ||
    !format.includes('extended->Samples.wValidBitsPerSample != 32') ||
    !format.includes('format->nChannels == 2 && format->cbSize == 0'))
  throw new Error('Float representation/layout must fail closed');
for (const field of ['wFormatTag', 'nChannels', 'nSamplesPerSec', 'nAvgBytesPerSec', 'nBlockAlign', 'wBitsPerSample', 'cbSize']) {
  if (!format.includes(`format->${field} == cached.${field}`)) throw new Error(`Cached WASAPI ${field} comparison absent`);
}
console.log('Upstream patch application verified.');
