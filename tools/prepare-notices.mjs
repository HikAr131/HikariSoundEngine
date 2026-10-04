// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
const root = path.resolve(import.meta.dirname, '..');
fs.copyFileSync(path.join(root, 'upstream/fxsound-app/LICENSE'), path.join(root, 'LICENSE'));
const driver = path.join(root, '.scratch/fxsound-driver');
const pin = 'c78fc6d031d16bd0a5dbbdff4871cfb8715d343d';
const rtf = execFileSync('git', ['-C', driver, 'show', `${pin}:license.rtf`]);
fs.writeFileSync(path.join(root, 'LICENSE-MS-LPL.rtf'), rtf);
const bs = String.fromCharCode(92);
const source = rtf.toString('utf8');
const paragraphs = [...source.matchAll(new RegExp(`${bs}${bs}ltrch ([^{}]*)`, 'g'))].map(m => m[1]
  .replaceAll(`${bs}ldblquote `, '"').replaceAll(`${bs}rdblquote `, '"')
  .replaceAll(`${bs}rquote `, "'"));
if (paragraphs.length < 15 || !paragraphs[0].includes('MICROSOFT LIMITED PUBLIC LICENSE')) throw Error('License extraction failed');
fs.writeFileSync(path.join(root, 'LICENSE-MS-LPL.txt'), paragraphs.join('\n\n') + '\n');
console.log(`Notices copied verbatim from app and MS-LPL text extracted from driver ${pin}.`);
