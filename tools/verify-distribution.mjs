// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
const root = path.resolve(import.meta.dirname, '..');
const exe = path.join(root, 'dist/HikariSoundEngine.exe');
const bytes = fs.readFileSync(exe);
const pe = bytes.readUInt32LE(60);
const optional = pe + 24;
if (bytes.toString('ascii', pe, pe + 2) !== 'PE' || bytes.readUInt16LE(pe + 4) !== 0x8664 || bytes.readUInt16LE(optional) !== 0x20b || bytes.readUInt16LE(optional + 68) !== 2) throw new Error('Expected PE x64 Windows GUI image');
const vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe';
const installation = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true }).trim();
const dumpbin = path.join(installation, 'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/dumpbin.exe');
const imports = execFileSync(dumpbin, ['/imports', exe], { encoding: 'utf8', windowsHide: true });
const dlls = [...new Set(imports.match(/\b[\w.-]+\.dll\b/gi))].sort();
if (dlls.some(value => /^(?:msvcp|vcruntime|ucrtbase|api-ms-win-crt)/i.test(value))) throw new Error('Dynamic VC runtime import detected');
const driverHashes = {};
for (const name of ['fxvad.sys', 'fxvad.inf', 'fxvadntamd64.cat']) {
  const distributed = fs.readFileSync(path.join(root, 'dist/drivers', name));
  const original = fs.readFileSync(path.join(root, 'upstream/fxsound-app/Installer/Drivers/Version14/win10/x64', name));
  if (!distributed.equals(original)) throw new Error(`Driver original was modified: ${name}`);
  driverHashes[name] = crypto.createHash('sha256').update(distributed).digest('hex');
}
if (process.argv[2] && !bytes.equals(fs.readFileSync(process.argv[2]))) throw new Error('Clean builds differ byte for byte');
console.log(JSON.stringify({ ok: true, x64: true, guiSubsystem: true, staticCrt: true, cleanBuildsIdentical: !!process.argv[2], signed: bytes.readUInt32LE(optional + 148) !== 0, exeBytes: bytes.length, exeSha256: crypto.createHash('sha256').update(bytes).digest('hex'), imports: dlls, driverHashes }, null, 2));
