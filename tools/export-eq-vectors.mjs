// SPDX-License-Identifier: AGPL-3.0-or-later
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { stripTypeScriptTypes } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = process.argv[2];
if (!sourceRoot) throw new Error('Usage: node tools/export-eq-vectors.mjs <1U-Hikari repository root>');
const relativeSources = ['web/src/types/eq.ts', 'web/src/utils/eqGenerator.ts'];
const contents = await Promise.all(relativeSources.map((name) => readFile(path.join(sourceRoot, name), 'utf8')));
const moduleUrl = (code) => `data:text/javascript;base64,${Buffer.from(code).toString('base64')}`;
const typesUrl = moduleUrl(stripTypeScriptTypes(contents[0]));
const generatorSource = stripTypeScriptTypes(contents[1]);
if (!generatorSource.includes("'../types/eq'")) throw new Error('Unexpected eqGenerator import; inspect the source before exporting');
const generatorUrl = moduleUrl(generatorSource.replace("'../types/eq'", JSON.stringify(typesUrl)));
const { computeParametricResponse, logFrequencyAxis } = await import(generatorUrl);
const labels = ['PK', 'LP', 'HP', 'LPQ', 'HPQ', 'BP', 'LS', 'HS', 'LSC', 'HSC', 'LSQ', 'HSQ', 'NO', 'AP'];
const configurations = [[20, -20, 0.1], [1000, 6, 0.7], [18000, -12, 10], [6000, 20, 1.41]];
const frequencies = logFrequencyAxis(256);
const rows = [];
for (const type of labels) {
  for (const [freq, gain, q] of configurations) {
    const response = computeParametricResponse(0, [{ enabled: true, type, freq, gain, q }], frequencies);
    if (response.length !== frequencies.length || !response.every(Number.isFinite)) throw new Error('Invalid source response');
    frequencies.forEach((testFrequency, i) => rows.push([type, freq, gain, q, testFrequency, response[i]]));
  }
}
const metadata = {
  schema: 'hikari-eq-vectors-v1',
  sampleRate: 48000,
  toleranceDb: 0.1,
  rowCount: rows.length,
  columns: ['type', 'freq', 'gain', 'q', 'testFrequency', 'expectedDb'],
  sources: relativeSources.map((file, i) => ({ file, sha256: createHash('sha256').update(contents[i]).digest('hex') })),
  approximation: ['BP', 'NO', 'AP', 'LSQ', 'HSQ'],
};
const header = JSON.stringify(metadata, null, 2).slice(0, -1).trimEnd();
const output = `${header},\n  "vectors": [\n${rows.map((row) => `    ${JSON.stringify(row)}`).join(',\n')}\n  ]\n}\n`;
await writeFile(path.join(root, 'tests/eq-vectors.json'), output);
console.log(`Exported ${rows.length} response points from the current TypeScript source`);
