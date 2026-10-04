// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const root = path.resolve(import.meta.dirname, '../dist');
const lines = [];
async function walk(directory, prefix = '') {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name, 'en'))) {
    const relative = prefix + entry.name;
    const file = path.join(directory, entry.name);
    if (entry.isSymbolicLink()) throw new Error('Distribution links are refused');
    if (entry.isDirectory()) await walk(file, relative + '/');
    else if (entry.isFile() && relative !== 'SHA256SUMS.txt') {
      const digest = crypto.createHash('sha256');
      for await (const chunk of fs.createReadStream(file)) digest.update(chunk);
      lines.push(`${digest.digest('hex')}  ${relative}`);
    }
  }
}
await walk(root);
fs.writeFileSync(path.join(root, 'SHA256SUMS.txt'), lines.sort().join('\n') + '\n');
console.log(`Distribution hashes updated (${lines.length} files).`);
