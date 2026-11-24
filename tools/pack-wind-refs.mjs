// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
// Packs the two wind recordings into the component file the engine reads:
//   node tools/pack-wind-refs.mjs <directory with wind-ref-1.f32 and wind-ref-2.f32>
// The result goes to assets/delta-wind-refs.bin, which is not part of the repository.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const root = path.resolve(import.meta.dirname, '..');
export const FILE_NAME = 'delta-wind-refs.bin';
export const SAMPLE_RATE = 48000;
export const CHANNELS = 2;
const MIN_FRAMES = 96000;

// The engine accepts exactly these recordings; the list is read from its source so that there is only one.
export function publishedDigests(source = fs.readFileSync(path.join(root, 'src/wind_reference.cpp'), 'utf8')) {
  const body = /publishedDigests\(\) \{([\s\S]*?)\n\}/.exec(source)?.[1] ?? '';
  const digests = [...body.matchAll(/"([0-9a-f]{64})"/g)].map(match => match[1]);
  if (!digests.length) throw new Error('No published digests found');
  return digests;
}

export function packSegments(segments, digests) {
  if (segments.length !== digests.length) throw new Error(`Expected ${digests.length} recordings`);
  const header = Buffer.alloc(12 + segments.length * 48);
  header.write('HKWR', 0, 'latin1');
  header.writeUInt32LE(1, 4);
  header.writeUInt32LE(segments.length, 8);
  const bodies = segments.map((bytes, index) => {
    const digest = crypto.createHash('sha256').update(bytes).digest('hex');
    if (digest !== digests[index]) throw new Error(`Recording ${index + 1} is not the published one: ${digest}`);
    if (bytes.length % (4 * CHANNELS) !== 0) throw new Error(`Recording ${index + 1} is not interleaved float32 stereo`);
    const samples = bytes.length / 4, frames = samples / CHANNELS;
    if (frames < MIN_FRAMES) throw new Error(`Recording ${index + 1} is too short`);
    const body = Buffer.alloc(samples * 2);
    for (let sample = 0; sample < samples; sample++) {
      const scaled = bytes.readFloatLE(sample * 4) * 32768;
      if (!Number.isInteger(scaled) || scaled < -32768 || scaled > 32767) throw new Error(`Recording ${index + 1} does not fit 16-bit exactly`);
      body.writeInt16LE(scaled, sample * 2);
    }
    const entry = 12 + index * 48;
    header.writeUInt32LE(SAMPLE_RATE, entry);
    header.writeUInt32LE(CHANNELS, entry + 4);
    header.writeBigUInt64LE(BigInt(frames), entry + 8);
    Buffer.from(digest, 'hex').copy(header, entry + 16);
    return body;
  });
  return Buffer.concat([header, ...bodies]);
}

// Throws unless the packed file holds exactly the published recordings.
export function verifyPacked(bytes, digests = publishedDigests()) {
  if (bytes.length < 12 || bytes.toString('latin1', 0, 4) !== 'HKWR' || bytes.readUInt32LE(4) !== 1 || bytes.readUInt32LE(8) !== digests.length)
    throw new Error('Wind recordings have an unexpected header');
  let offset = 12 + digests.length * 48;
  digests.forEach((digest, index) => {
    const entry = 12 + index * 48;
    const frames = Number(bytes.readBigUInt64LE(entry + 8)), size = frames * CHANNELS * 2;
    if (bytes.readUInt32LE(entry) !== SAMPLE_RATE || bytes.readUInt32LE(entry + 4) !== CHANNELS || bytes.toString('hex', entry + 16, entry + 48) !== digest ||
        offset + size > bytes.length) throw new Error(`Wind recording ${index + 1} has an unexpected entry`);
    const floats = Buffer.alloc(size * 2);
    for (let sample = 0; sample < size / 2; sample++) floats.writeFloatLE(bytes.readInt16LE(offset + sample * 2) / 32768, sample * 4);
    if (crypto.createHash('sha256').update(floats).digest('hex') !== digest) throw new Error(`Wind recording ${index + 1} is altered`);
    offset += size;
  });
  if (offset !== bytes.length) throw new Error('Wind recordings carry trailing bytes');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const directory = process.argv[2];
  if (!directory) throw new Error('Usage: node tools/pack-wind-refs.mjs <directory with the .f32 recordings>');
  const digests = publishedDigests();
  const packed = packSegments(digests.map((_, index) => fs.readFileSync(path.join(directory, `wind-ref-${index + 1}.f32`))), digests);
  verifyPacked(packed, digests);
  const target = path.join(root, 'assets', FILE_NAME);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, packed);
  console.log(JSON.stringify({ ok: true, file: target, bytes: packed.length, sha256: crypto.createHash('sha256').update(packed).digest('hex') }, null, 2));
}
