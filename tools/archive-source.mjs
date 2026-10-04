// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '..');
const appPin = 'd8e7a23d37ed5939c2a3090a1c1756c7f2500b17';
const driverPin = 'c78fc6d031d16bd0a5dbbdff4871cfb8715d343d';
const argv = new Set(process.argv.slice(2));
const allowedArgs = new Set(['--draft', '--list', '--verify-extracted']);
for (const arg of argv) if (!allowedArgs.has(arg)) throw Error(`Unknown option: ${arg}`);
const rootFiles = new Set(['.gitattributes', '.gitignore', '.gitmodules', 'CMakeLists.txt', 'LICENSE',
  'LICENSE-MS-LPL.rtf', 'LICENSE-MS-LPL.txt', 'MODIFICATIONS.md', 'README.md', 'SOURCE.txt',
  'THIRD-PARTY.md', 'app.manifest', 'build.ps1']);
const sourceDirs = new Set(['src', 'tests', 'patches', 'tools', 'docs']);
const required = ['CMakeLists.txt', 'build.ps1', 'app.manifest', 'LICENSE', 'LICENSE-MS-LPL.txt',
  'LICENSE-MS-LPL.rtf', 'SOURCE.txt', 'THIRD-PARTY.md', 'patches/01-headless-host.patch',
  'tests/eq-vectors.json', 'tests/graphic-vectors.json', 'tools/archive-source.mjs',
  'tools/verify-upstream.mjs', 'docs/source-archive.md'];
const forbiddenDirs = new Set(['.git', 'build', 'dist', '.scratch', 'plan', 'node_modules', '.claude', '.hotfix-keys']);

function run(executable, args, cwd = root) {
  return execFileSync(executable, args, { cwd, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024,
    windowsHide: true, timeout: 180000, stdio: ['ignore', 'pipe', 'pipe'] });
}
function git(args, cwd = root) { return run('git', args, cwd); }
function safeRelative(file) {
  if (!file || file.includes(String.fromCharCode(92)) || file.startsWith('/') || file.includes(':')) return false;
  return file.split('/').every(part => part && part !== '.' && part !== '..');
}
function assertPublicPath(file) {
  if (!safeRelative(file)) throw Error(`Unsafe archive path: ${file}`);
  if (file.split('/').some(part => forbiddenDirs.has(part))) throw Error(`Private/generated directory: ${file}`);
  if (/(?:^|\/)\.env(?:\.|$)|\.(?:pfx|p12|pem|key|user)$/i.test(file)) throw Error(`Private file type: ${file}`);
}
async function hashFile(file) {
  const hash = crypto.createHash('sha256');
  for await (const bytes of fs.createReadStream(file)) hash.update(bytes);
  return hash.digest('hex');
}
async function walk(directory, prefix = '', ignoredRootDirs = new Set()) {
  const files = [];
  for (const entry of await fsp.readdir(directory, { withFileTypes: true })) {
    const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (!prefix && entry.isDirectory() && ignoredRootDirs.has(entry.name)) continue;
    if (entry.isSymbolicLink()) throw Error(`Reparse/symbolic link refused: ${relative}`);
    if (entry.isDirectory()) files.push(...await walk(path.join(directory, entry.name), relative, ignoredRootDirs));
    else if (entry.isFile()) files.push(relative);
    else throw Error(`Unsupported file type: ${relative}`);
  }
  return files.sort();
}
async function verifyExtracted(directory) {
  const manifest = JSON.parse(await fsp.readFile(path.join(directory, 'SOURCE-ARCHIVE.json'), 'utf8'));
  if (manifest.format !== 1 || manifest.app.commit !== appPin || !Array.isArray(manifest.files))
    throw Error('Source manifest format/pin mismatch');
  const seen = new Set();
  for (const item of manifest.files) {
    assertPublicPath(item.path);
    if (seen.has(item.path) || !/^[a-f0-9]{64}$/.test(item.sha256) || !Number.isSafeInteger(item.bytes) || item.bytes < 0)
      throw Error(`Invalid source manifest entry: ${item.path}`);
    seen.add(item.path);
    const file = path.join(directory, ...item.path.split('/'));
    const stat = await fsp.lstat(file);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size !== item.bytes || await hashFile(file) !== item.sha256)
      throw Error(`Source hash mismatch: ${item.path}`);
  }
  const actual = (await walk(directory, '', new Set(['build', 'dist']))).filter(file => file !== 'SOURCE-ARCHIVE.json');
  if (actual.length !== seen.size || actual.some(file => !seen.has(file))) throw Error('Unexpected file in source snapshot');
  for (const file of required) if (!seen.has(file)) throw Error(`Missing helper build input: ${file}`);
  for (const file of ['dsp/DfxDsp.vcxproj', 'audiopassthru/audiopassthru.vcxproj',
    'Installer/Drivers/Version14/win10/x64/fxvad.sys', 'Installer/Drivers/Version14/win10/x64/fxvad.inf',
    'Installer/Drivers/Version14/win10/x64/fxvadntamd64.cat']) {
    if (!seen.has(`upstream/fxsound-app/${file}`)) throw Error(`Missing pinned upstream input: ${file}`);
  }
  if (!seen.has('build-from-source.ps1')) throw Error('Offline build script is absent');
  return manifest;
}

if (argv.has('--verify-extracted')) {
  const manifest = await verifyExtracted(root);
  console.log(JSON.stringify({ ok: true, verifiedFiles: manifest.files.length, appPin, draft: manifest.draft }));
  process.exit(0);
}

const draft = argv.has('--draft');
const candidateFiles = git(['ls-files', '-z', '--cached', ...(draft ? ['--others', '--exclude-standard'] : [])])
  .split(String.fromCharCode(0)).filter(Boolean);
const helperFiles = [...new Set(candidateFiles)].filter(file => rootFiles.has(file) || sourceDirs.has(file.split('/')[0])).sort();
for (const file of helperFiles) assertPublicPath(file);
for (const file of required) if (!helperFiles.includes(file)) throw Error(`Required source is not selected/tracked: ${file}; use --draft only for a local development snapshot`);
if (!draft && git(['status', '--porcelain']).trim()) throw Error('A final source archive requires a clean, committed helper checkout');
let helperCommit = null;
try { helperCommit = git(['rev-parse', '--verify', 'HEAD']).trim(); } catch { if (!draft) throw Error('Helper has no commit'); }

function inventory(repo, pin, prefix, requireClean = true) {
  if (git(['rev-parse', 'HEAD'], repo).trim() !== pin || (requireClean && git(['status', '--porcelain'], repo).trim()))
    throw Error(`Pinned upstream must be clean: ${prefix}`);
  const entries = git(['ls-tree', '-rz', '--full-tree', pin], repo).split(String.fromCharCode(0)).filter(Boolean);
  const files = [], gitlinks = [];
  for (const line of entries) {
    const match = /^(\d+) (blob|commit) ([a-f0-9]{40})\t(.+)$/.exec(line);
    if (!match) throw Error(`Unsupported git tree entry: ${line}`);
    const [, mode, type, oid, file] = match;
    assertPublicPath(`${prefix}/${file}`);
    if (type === 'commit') gitlinks.push({ path: `${prefix}/${file}`, commit: oid });
    else {
      if (mode !== '100644' && mode !== '100755') throw Error(`Unsupported upstream link/mode: ${file}`);
      files.push(`${prefix}/${file}`);
    }
  }
  return { files, gitlinks };
}
const appRepo = path.join(root, 'upstream/fxsound-app');
const app = inventory(appRepo, appPin, 'upstream/fxsound-app');
const driverRepo = path.join(root, '.scratch/fxsound-driver');
const driver = fs.existsSync(path.join(driverRepo, '.git')) ? inventory(driverRepo, driverPin, 'upstream/fxsound-driver-source', false) : null;
const list = { helper: helperFiles, app: { commit: appPin, files: app.files, excludedNestedGitlinks: app.gitlinks },
  driver: driver ? { commit: driverPin, files: driver.files, excludedNestedGitlinks: driver.gitlinks } : null,
  generated: ['build-from-source.ps1', 'SOURCE-ARCHIVE.json'], draft };
if (argv.has('--list')) { console.log(JSON.stringify(list, null, 2)); process.exit(0); }

const scratch = await fsp.mkdtemp(path.join(os.tmpdir(), 'hikari-source-archive-'));
const staging = path.join(scratch, 'snapshot');
const extracted = path.join(scratch, 'verify');
const output = path.join(root, 'dist/HikariSoundEngine-source.zip');
await fsp.mkdir(staging);
try {
  for (const file of helperFiles) {
    const source = path.join(root, ...file.split('/'));
    const stat = await fsp.lstat(source);
    if (!stat.isFile() || stat.isSymbolicLink()) throw Error(`Non-regular helper source: ${file}`);
    const destination = path.join(staging, ...file.split('/'));
    await fsp.mkdir(path.dirname(destination), { recursive: true });
    await fsp.copyFile(source, destination);
  }
  async function archiveUpstream(repo, pin, prefix, expected) {
    const tarFile = path.join(scratch, `${prefix.split('/').at(-1)}.tar`);
    git(['archive', '--format=tar', `--prefix=${prefix}/`, `--output=${tarFile}`, pin], repo);
    run('tar.exe', ['-xf', tarFile, '-C', staging]);
    for (const file of expected) if (!fs.existsSync(path.join(staging, ...file.split('/')))) throw Error(`Git archive omitted tracked source: ${file}`);
  }
  await archiveUpstream(appRepo, appPin, 'upstream/fxsound-app', app.files);
  if (driver) await archiveUpstream(driverRepo, driverPin, 'upstream/fxsound-driver-source', driver.files);
  const buildOriginal = await fsp.readFile(path.join(staging, 'build.ps1'), 'utf8');
  const preflightStart = buildOriginal.indexOf('    $head = & git -C $upstreamRoot rev-parse HEAD');
  const preflightEnd = buildOriginal.indexOf('    New-Item -ItemType Directory -Path $buildRoot', preflightStart);
  if (preflightStart < 0 || preflightEnd < 0) throw Error('Build preflight changed; archive builder must be reviewed');
  const offlineBuild = '# Generated local source-snapshot build entry. Original build.ps1 is preserved.\n' +
    buildOriginal.slice(0, preflightStart) + "    Invoke-Checked 'node' @('tools/archive-source.mjs', '--verify-extracted')\n" +
    buildOriginal.slice(preflightEnd);
  if ([...Buffer.from(offlineBuild)].some(byte => byte > 127)) throw Error('Generated PowerShell must be ASCII');
  await fsp.writeFile(path.join(staging, 'build-from-source.ps1'), offlineBuild);
  const files = [];
  for (const file of await walk(staging)) {
    assertPublicPath(file);
    const source = path.join(staging, ...file.split('/'));
    files.push({ path: file, bytes: (await fsp.stat(source)).size, sha256: await hashFile(source) });
  }
  const manifest = { format: 1, draft, helperCommit, helperFiles, app: list.app, driver: list.driver,
    omittedNestedResources: 'Official GUI-only Resources gitlink is recorded but not a helper build input.',
    driverBinarySourceMatch: 'NOT VERIFIED; pinned unmodified signed binaries and reference driver source are included separately.', files };
  await fsp.writeFile(path.join(staging, 'SOURCE-ARCHIVE.json'), JSON.stringify(manifest, null, 2) + '\n');
  await verifyExtracted(staging);
  const zipTemporary = path.join(scratch, 'source.zip');
  const zipper = path.join(scratch, 'zip.ps1');
  await fsp.writeFile(zipper, `param([string]$Source,[string]$Zip,[string]$Extract)\n$ErrorActionPreference = 'Stop'\nAdd-Type -AssemblyName System.IO.Compression.FileSystem\n[IO.Compression.ZipFile]::CreateFromDirectory($Source,$Zip,[IO.Compression.CompressionLevel]::Optimal,$false)\n[IO.Compression.ZipFile]::ExtractToDirectory($Zip,$Extract)\n`);
  run('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', zipper, '-Source', staging, '-Zip', zipTemporary, '-Extract', extracted]);
  const extractedManifest = await verifyExtracted(extracted);
  // Verify project source lists and patch applicability using the actual extracted files.
  const patchRoot = path.join(extracted, 'build/upstream');
  await fsp.mkdir(patchRoot, { recursive: true });
  for (const directory of ['dsp', 'audiopassthru'])
    await fsp.cp(path.join(extracted, 'upstream/fxsound-app', directory), path.join(patchRoot, directory), { recursive: true });
  for (const patchFile of helperFiles.filter(file => file.startsWith('patches/') && file.endsWith('.patch')).sort()) {
    git(['apply', '--check', '--unsafe-paths', '--directory=build/upstream', patchFile], extracted);
    git(['apply', '--unsafe-paths', '--directory=build/upstream', patchFile], extracted);
  }
  run('node', ['tools/verify-upstream.mjs'], extracted);
  for (const project of ['dsp/DfxDsp.vcxproj', 'audiopassthru/audiopassthru.vcxproj']) {
    const projectFile = path.join(patchRoot, ...project.split('/'));
    const xml = await fsp.readFile(projectFile, 'utf8');
    for (const match of xml.matchAll(/<ClCompile Include="([^"]+)"/g)) {
      const relative = match[1].split(String.fromCharCode(92)).join('/');
      if (!fs.existsSync(path.resolve(path.dirname(projectFile), relative))) throw Error(`Missing compile input after extraction: ${project}/${relative}`);
    }
  }
  await fsp.mkdir(path.dirname(output), { recursive: true });
  await fsp.copyFile(zipTemporary, output);
  console.log(JSON.stringify({ ok: true, output, draft, files: extractedManifest.files.length,
    bytes: (await fsp.stat(output)).size, sha256: await hashFile(output), verification: 'full extraction, every-file SHA256, upstream project compile inputs, and patch applicability' }, null, 2));
} finally {
  const resolved = path.resolve(scratch);
  if (path.dirname(resolved) !== path.resolve(os.tmpdir()) || !path.basename(resolved).startsWith('hikari-source-archive-'))
    throw Error('Refusing unsafe temporary-directory cleanup');
  await walk(resolved);
  await fsp.rm(resolved, { recursive: true, force: true });
}
