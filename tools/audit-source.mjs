// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
const excluded = new Set(['.git', 'upstream', 'build', 'dist', '.scratch', 'plan']);
const failures = [];
let files = 0;
function visit(directory) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    if (excluded.has(entry.name)) continue;
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) visit(file);
    else if (/\.(cpp|h|mjs|ps1|md|txt|json|patch|manifest)$/.test(entry.name)) {
      const bytes = fs.readFileSync(file); ++files;
      for (let i = 0; i < bytes.length; ++i) {
        const value = bytes[i];
        if (value < 32 && value !== 9 && value !== 10 && value !== 13) failures.push(`${file}:${i} control byte ${value}`);
      }
      if (entry.name.endsWith('.ps1') && bytes.some(value => value > 127)) failures.push(`${file}: non-ASCII delivery script`);
    }
  }
}
visit(process.cwd());
if (failures.length) { console.error(failures.join('\n')); process.exitCode = 1; }
else console.log(`Source byte audit passed (${files} files).`);
