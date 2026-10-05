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
// The helper runs from a user-writable directory: a static import that is not a KnownDLL could be
// shadowed by a file placed beside it, so every other DLL must be delay-loaded after main restricts
// the search path to System32.
const sectionStart = header => imports.indexOf(`Section contains the following ${header}:`);
const staticStart = sectionStart('imports'), delayStart = sectionStart('delay load imports'), summaryStart = imports.indexOf('  Summary');
if (staticStart < 0 || summaryStart < staticStart) throw new Error('Static import table not found');
const staticText = imports.slice(staticStart, delayStart > staticStart ? delayStart : summaryStart);
const delayText = delayStart > staticStart ? imports.slice(delayStart, summaryStart > delayStart ? summaryStart : imports.length) : '';
const dllNames = text => [...new Set([...text.matchAll(/^ {4}([\w.-]+\.dll)\s*$/gim)].map(match => match[1].toLowerCase()))].sort();
const staticImports = dllNames(staticText), delayLoadImports = dllNames(delayText);
// Long-standing KnownDLLs on Windows 10 and 11, cross-checked against this machine's list below.
const knownDlls = new Set(['advapi32.dll', 'combase.dll', 'gdi32.dll', 'kernel32.dll', 'ole32.dll', 'oleaut32.dll',
  'rpcrt4.dll', 'sechost.dll', 'shell32.dll', 'shlwapi.dll', 'user32.dll', 'ws2_32.dll']);
const knownDllsKey = ['HKLM:', 'SYSTEM', 'CurrentControlSet', 'Control', 'Session Manager', 'KnownDLLs'].join(String.fromCharCode(92));
const localKnownDlls = new Set(execFileSync('powershell.exe', ['-NoProfile', '-Command',
  `(Get-ItemProperty -LiteralPath '${knownDllsKey}').PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { $_.Value }`],
  { encoding: 'utf8', windowsHide: true }).split(/\r?\n/).map(value => value.trim().toLowerCase()).filter(Boolean));
for (const dll of staticImports) {
  if (!knownDlls.has(dll) || !localKnownDlls.has(dll)) throw new Error(`Static import is not a KnownDLL and must be delay-loaded: ${dll}`);
}
for (const dll of ['cfgmgr32.dll', 'wtsapi32.dll']) {
  if (!delayLoadImports.includes(dll)) throw new Error(`Expected delay-loaded import: ${dll}`);
}
if (!/^\s+[0-9A-F]+\s+SetDefaultDllDirectories\s*$/m.test(staticText)) throw new Error('SetDefaultDllDirectories is not imported');
const mainSource = fs.readFileSync(path.join(root, 'src/main.cpp'), 'utf8').replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '');
if (!/int\s+WINAPI\s+wWinMain\s*\([^)]*\)\s*\{\s*if\s*\(\s*!\s*SetDefaultDllDirectories\s*\(\s*LOAD_LIBRARY_SEARCH_SYSTEM32\s*\)\s*\)/.test(mainSource))
  throw new Error('wWinMain must restrict the DLL search to System32 before anything else');
// The upstream repository stores fxvad.inf with LF; the signed catalog covers the CRLF form.
const signedLineEndings = bytes => {
  if (bytes.includes(13)) throw new Error('Upstream fxvad.inf already contains CR; review the line ending restoration');
  return Buffer.from(bytes.toString('latin1').split(String.fromCharCode(10)).join(String.fromCharCode(13, 10)), 'latin1');
};
const driverHashes = {};
for (const name of ['fxvad.sys', 'fxvad.inf', 'fxvadntamd64.cat']) {
  const distributed = fs.readFileSync(path.join(root, 'dist/drivers', name));
  const original = fs.readFileSync(path.join(root, 'upstream/fxsound-app/Installer/Drivers/Version14/win10/x64', name));
  const expected = name === 'fxvad.inf' ? signedLineEndings(original) : original;
  if (!distributed.equals(expected)) throw new Error(`Driver original was modified: ${name}`);
  driverHashes[name] = crypto.createHash('sha256').update(distributed).digest('hex');
}
// Windows installs the package only when the INF and the driver are both listed in the signed catalog;
// verify them under the driver policy that Plug and Play applies.
const kitsBin = 'C:/Program Files (x86)/Windows Kits/10/bin';
const kitVersions = fs.existsSync(kitsBin) ? fs.readdirSync(kitsBin).filter(entry => /^\d+(?:\.\d+){3}$/.test(entry)) : [];
kitVersions.sort((a, b) => a.split('.').map(Number).reduce((order, part, index) => order || part - Number(b.split('.')[index]), 0));
const signtool = kitVersions.reverse().map(version => path.join(kitsBin, version, 'x64', 'signtool.exe')).find(file => fs.existsSync(file));
if (!signtool) throw new Error('signtool.exe from the Windows SDK is required to verify the driver catalog');
const driverPolicy = '{F750E6C3-38EE-11D1-85E5-00C04FC295EE}';
for (const name of ['fxvad.inf', 'fxvad.sys']) {
  try {
    execFileSync(signtool, ['verify', '/q', '/pg', driverPolicy, '/c', path.join(root, 'dist/drivers/fxvadntamd64.cat'), path.join(root, 'dist/drivers', name)], { stdio: 'pipe', windowsHide: true });
  } catch { throw new Error(`Driver file is not covered by the signed catalog: ${name}`); }
}
if (process.argv[2] && !bytes.equals(fs.readFileSync(process.argv[2]))) throw new Error('Clean builds differ byte for byte');
console.log(JSON.stringify({ ok: true, x64: true, guiSubsystem: true, staticCrt: true, cleanBuildsIdentical: !!process.argv[2], signed: bytes.readUInt32LE(optional + 148) !== 0, exeBytes: bytes.length, exeSha256: crypto.createHash('sha256').update(bytes).digest('hex'), staticImports, delayLoadImports, dllSearch: 'System32 only', driverHashes, driverCatalogVerified: true }, null, 2));
