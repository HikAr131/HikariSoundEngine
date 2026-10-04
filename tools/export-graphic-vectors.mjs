// SPDX-License-Identifier: AGPL-3.0-or-later
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { stripTypeScriptTypes } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = process.argv[2];
if (!sourceRoot) throw new Error('Usage: node tools/export-graphic-vectors.mjs <1U-Hikari repository root>');
const files = ['web/src/types/eq.ts', 'web/src/data/eqPresets.ts'];
const source = await Promise.all(files.map((file) => readFile(path.join(sourceRoot, file), 'utf8')));
const moduleUrl = (code) => `data:text/javascript;base64,${Buffer.from(code).toString('base64')}`;
const typeUrl = moduleUrl(stripTypeScriptTypes(source[0]));
const presetModule = stripTypeScriptTypes(source[1]);
if (!presetModule.includes("'../types/eq'")) throw new Error('Unexpected preset import');
const { GRAPHIC_EQ_FREQUENCIES } = await import(typeUrl);
const { BUILTIN_EQ_PRESETS, builtinPresetToParams } = await import(moduleUrl(presetModule.replace("'../types/eq'", JSON.stringify(typeUrl))));
if (BUILTIN_EQ_PRESETS.length !== 13) throw new Error('Expected thirteen built-in presets');
const frequencies = Array.from({ length: 512 }, (_, i) => 30 * Math.pow(16000 / 30, i / 511));
const target = (points, frequency) => {
  if (frequency <= points[0].freq) return points[0].gain;
  if (frequency >= points.at(-1).freq) return points.at(-1).gain;
  const index = points.findIndex((point) => point.freq >= frequency);
  const left = points[index - 1], right = points[index];
  return left.gain + (right.gain - left.gain) * Math.log(frequency / left.freq) / Math.log(right.freq / left.freq);
};
const presets = BUILTIN_EQ_PRESETS.map((preset) => ({
  key: preset.key,
  points: builtinPresetToParams(preset).graphic,
  enforceThreshold: true,
}));
const synthetic = [
  ['synthetic-all-plus-12', GRAPHIC_EQ_FREQUENCIES.map(() => 12)],
  ['synthetic-all-minus-12', GRAPHIC_EQ_FREQUENCIES.map(() => -12)],
  ['synthetic-alternating', GRAPHIC_EQ_FREQUENCIES.map((_, i) => i % 2 ? -12 : 12)],
  ['synthetic-single-peak', GRAPHIC_EQ_FREQUENCIES.map((_, i) => i === 4 ? 12 : 0)],
  ['synthetic-step', GRAPHIC_EQ_FREQUENCIES.map((_, i) => i < 5 ? 12 : -12)],
];
for (const [key, gains] of synthetic) presets.push({
  key,
  points: GRAPHIC_EQ_FREQUENCIES.map((freq, i) => ({ freq, gain: gains[i] })),
  enforceThreshold: false,
});
for (const preset of presets) {
  preset.frequencies = frequencies;
  preset.expectedDb = frequencies.map((frequency) => target(preset.points, frequency));
}
const output = {
  schema: 'hikari-graphic-vectors-v1',
  sampleRate: 48000,
  toleranceDb: 1,
  frequencyMin: 30,
  frequencyMax: 16000,
  interpolation: 'piecewise-linear-dB-on-log-frequency-with-constant-ends',
  referenceEvidence: {
    repository: 'https://github.com/mirror/equalizerapo',
    commit: '53d885f7f1a097b457e17a5206b7d60f647877a8',
    file: 'helpers/GainIterator.cpp',
    lines: '73-91',
  },
  sources: files.map((file, i) => ({ file, sha256: createHash('sha256').update(source[i]).digest('hex') })),
  presets,
};
await writeFile(path.join(root, 'tests/graphic-vectors.json'), `${JSON.stringify(output, null, 2)}\n`);
console.log(`Exported ${presets.length} graphic curves (${BUILTIN_EQ_PRESETS.length} built-in and ${synthetic.length} synthetic)`);
