// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
const root = path.resolve(import.meta.dirname, '..');
const upstream = path.join(root, 'upstream/fxsound-app');
const stage = path.join(root, 'plan/patch-generation');
fs.mkdirSync(stage, {recursive:true});
fs.mkdirSync(path.join(root, 'patches'), {recursive:true});
const changes = new Map();
function modify(file, transform) {
  const original = fs.readFileSync(path.join(upstream, file), 'utf8');
  const eol = original.includes('\r\n') ? '\r\n' : '\n';
  const transformed = transform(original.replaceAll('\r\n', '\n'), '\n');
  const next = ('// Hikari modification 2026-10-04: isolated headless host integration; see MODIFICATIONS.md.\n' + transformed).replaceAll('\n', eol);
  if (next === original) throw new Error(`Unchanged patch: ${file}`);
  changes.set(file, {original, next});
}
function replaceOne(text, oldText, nextText) {
  if (!text.includes(oldText)) throw new Error(`Missing patch anchor: ${oldText}`);
  return text.replace(oldText, nextText);
}
modify('dsp/DfxDspPrivate.cpp', text => {
  text = text.replaceAll('L"DFX"', 'L"Hikari1U\\\\SoundEngine"');
  text = replaceOne(text, '#include "u_DfxDsp.h"', '#include <stdexcept>\n#include "u_DfxDsp.h"');
  text = replaceOne(text, 'dfxp_handle_ = NULL;', 'dfxp_handle_ = NULL;\n\tpreset_list_handle_ = NULL;');
  return replaceOne(text, 'MessageBox(NULL, L"TTEST", L"TEST", MB_OK);', 'throw std::runtime_error("DfxDsp initialization failed");');
});
modify('audiopassthru/src/sndDevices/sndDevicesReg.cpp', text => text.replaceAll('L"DFX"', 'L"Hikari1U\\\\SoundEngine"'));
for (const file of ['dsp/ptutil/include/codedefs.h','audiopassthru/include/codedefs.h']) modify(file, text => {
  return '#include "hikari_upstream_logging.h"\n' + text.replace(/MessageBoxW\([^;]+;/g, 'hikariLogUpstreamError(wcp_file);')
    .replace(/MessageBoxA\([^;]+;/g, 'hikariLogUpstreamErrorA(cp_file);');
});
modify('audiopassthru/src/operatingSystem/operatingSystem.cpp', text =>
  replaceOne(text, 'MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK);', 'hikariLogUpstreamError((LPCTSTR)lpDisplayBuf);'));
modify('audiopassthru/src/sndDevices/sndDevicesInit.cpp', text =>
  replaceOne(text, '#include "BladeMP3EncDLL.h"', '// Unused MP3 encoder header excluded from the headless host.'));
modify('audiopassthru/src/AudioPassthru/AudioPassthruPrivate.cpp', text => {
  text = replaceOne(text, '#include "u_AudioPassthru.h"', '#include "hikari_upstream_hooks.h"\n#include "u_AudioPassthru.h"');
  const start = text.indexOf('\t\t\tif (p_dfx_dsp_->setSignalFormat(');
  const end = text.indexOf('\n\t\t\t// 2016-08-25:', start);
  if (start < 0 || end < 0) throw new Error('Missing DSP format call anchors');
  text = text.slice(0,start) + text.slice(end);
  return replaceOne(text,
    'p_dfx_dsp_->processAudio((short int *)fp_buffer, (short int *)fp_buffer, numSampleSets, i_check_for_duplicate_buffers);',
    'hikariProcessAudio(p_dfx_dsp_, fp_buffer, numSampleSets, pwfx->wBitsPerSample, pwfx->nChannels, pwfx->nSamplesPerSec, i_valid_bits, i_check_for_duplicate_buffers);');
});
modify('audiopassthru/src/sndDevices/sndDevicesSet.cpp', text => {
  text = replaceOne(text, '#include "codedefs.h"', '#include "hikari_upstream_hooks.h"\n#include "codedefs.h"');
  return replaceOne(text, 'case SND_DEVICES_DEFAULT :',
    'case SND_DEVICES_DEFAULT :\n\t\tif (!hikariAllowDefaultSwitch(cast_handle->pwszID[device_index_num])) return OKAY;');
});
modify('audiopassthru/src/sndDevices/sndDevicesSetupDevices.cpp', text => {
  text = replaceOne(text, '#include "codedefs.h"', '#include "hikari_upstream_hooks.h"\n#include "codedefs.h"');
  for (const kind of ['Capture', 'Playback']) {
    text = replaceOne(text, `cast_handle->wfx${kind} = *pwfx;`,
      'if (!hikariValidateAudioInitialization(pwfx, pwfx))\n\t{\n' +
      '\t\tCoTaskMemFree(pwfx);\n\t\t*ip_status = SND_DEVICES_AUDIO_CLIENT_INIT_FAILED;\n' +
      '\t\tSND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_AUDIO_CLIENT_INIT_FAILED);\n\t}\n\t' +
      `cast_handle->wfx${kind} = *pwfx;`);
  }
  let calls = 0;
  text = text.replace(/hr = (cast_handle->pAudioClient(Capture|Playback))->Initialize\(([\s\S]*?)\);/g,
    (_, client, kind, args) => {
      ++calls;
      const actual = args.includes('pClosestMatch') ? 'pClosestMatch' : 'pwfx';
      return `hr = hikariValidateAudioInitialization(${actual}, &cast_handle->wfx${kind})\n` +
        `\t\t? ${client}->Initialize(${args}) : AUDCLNT_E_UNSUPPORTED_FORMAT;`;
    });
  if (calls !== 4) throw new Error(`Expected four WASAPI Initialize sites, found ${calls}`);
  return text;
});
modify('audiopassthru/src/sndDevices/sndDevicesDeviceCallbacks.cpp', text => {
  text = replaceOne(text, '#include "codedefs.h"', '#include "hikari_upstream_hooks.h"\n#include "codedefs.h"');
  return replaceOne(text, '{  \n  struct sndDevicesHdlType *cast_handle;',
    '{  \n  hikariOnDefaultDeviceChanged(static_cast<int>(flow), static_cast<int>(role), pwstrDeviceId);\n  struct sndDevicesHdlType *cast_handle;');
});
modify('audiopassthru/src/sndDevices/sndDevicesImplementDeviceRules.cpp', text => {
  text = replaceOne(text, '#include "codedefs.h"', '#include "hikari_upstream_hooks.h"\n#include "codedefs.h"');
  return replaceOne(text, 'PlaybackDeviceIsSelected:  // Label to jump to when the playback device num has been determined.',
    'PlaybackDeviceIsSelected:  // Label to jump to when the playback device num has been determined.\n' +
    '\t{\n\t\twchar_t preferredOutput[PT_MAX_GENERIC_STRLEN] = {};\n' +
    '\t\tif (hikariGetPreferredOutput(preferredOutput, PT_MAX_GENERIC_STRLEN))\n\t\t{\n' +
    '\t\t\tint preferredIndex = SND_DEVICES_DEVICE_NOT_PRESENT;\n' +
    '\t\t\tif (sndDevices_UtilsGetIndexFromID(hp_sndDevices, preferredOutput, &preferredIndex) != OKAY) return NOT_OKAY;\n' +
    '\t\t\tif (preferredIndex == SND_DEVICES_DEVICE_NOT_PRESENT || preferredIndex == cast_handle->dfxDeviceNum)\n\t\t\t{\n' +
    '\t\t\t\t*ip_resultFlag = SND_DEVICES_DEVICE_NOT_PRESENT;\n' +
    '\t\t\t\tSND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_DEVICE_NOT_PRESENT);\n\t\t\t}\n' +
    '\t\t\tcast_handle->playbackDeviceNum = preferredIndex;\n\t\t}\n\t}');
});
for (const file of ['audiopassthru/src/reg/regWithKeyname.cpp','audiopassthru/src/reg/regWithoutKeyname.cpp','audiopassthru/src/reg/regRecursiveDelete.cpp']) modify(file, text => {
  return replaceOne(text, '#include "codedefs.h"', '#include "hikari_upstream_hooks.h"\n#define RegCreateKeyExW hikariRegCreateKeyExW\n#define RegOpenKeyExW hikariRegOpenKeyExW\n#define RegDeleteKeyW hikariRegDeleteKeyW\n#include "codedefs.h"');
});
let patch = '# Hikari upstream integration: isolated registry, headless failures, host DSP/default hooks.\n';
for (const [file,{original,next}] of changes) {
  const before = path.join(stage,'before',file), after=path.join(stage,'after',file);
  fs.mkdirSync(path.dirname(before),{recursive:true}); fs.mkdirSync(path.dirname(after),{recursive:true});
  fs.writeFileSync(before,original); fs.writeFileSync(after,next);
  const result = spawnSync('git',['diff','--no-index','--no-ext-diff','--no-color','--',before,after],{encoding:'utf8',maxBuffer:32*1024*1024});
  if(result.status!==1) throw new Error(result.stderr || 'Patch generation failed');
  const lines=result.stdout.split('\n');
  lines[0]=`diff --git a/${file} b/${file}`;
  const minus=lines.findIndex(line=>line.startsWith('--- '));
  const plus=lines.findIndex(line=>line.startsWith('+++ '));
  lines[minus]=`--- a/${file}`; lines[plus]=`+++ b/${file}`;
  patch+=lines.join('\n');
}
fs.writeFileSync(path.join(root,'patches/01-headless-host.patch'),patch);
console.log(`Generated checked patch for ${changes.size} files; upstream tree remains untouched.`);
