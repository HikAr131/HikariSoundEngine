// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
import path from 'node:path';

const appPrefix = 'upstream/fxsound-app/';
const driverSourcePrefix = 'upstream/fxsound-driver-source/';
const driverPackagePrefix = `${appPrefix}Installer/Drivers/`;
const redistributedDriverDirectory = `${driverPackagePrefix}Version14/win10/x64/`;
const omittedExtensions = ['exe', 'dll', 'pdb', 'ilk', 'iobj', 'ipdb', 'lib', 'obj', 'exp', 'msi', 'msm', 'msp',
  'cab', 'sys', 'cat', 'syso'];

export const redistributedDriverNames = Object.freeze(['fxvad.sys', 'fxvad.inf', 'fxvadntamd64.cat']);
export const redistributedDriverFiles = Object.freeze(redistributedDriverNames.map(name => redistributedDriverDirectory + name));
export const omissionPolicy = 'Prebuilt upstream binaries that are not helper build inputs are omitted: files with extension ' +
  `${omittedExtensions.map(extension => `.${extension}`).join(' ')} (any case) and .zip files under ${driverPackagePrefix}; ` +
  `${redistributedDriverNames.join(', ')} in ${redistributedDriverDirectory} are always kept. ` +
  'Each omitted entry is the unmodified git blob (git ls-tree -l) at the pinned upstream commit.';

// Archive paths of upstream prebuilt artifacts left out of the source archive; helper files never match.
export function isOmittedUpstreamBinary(archivePath) {
  if (!archivePath.startsWith(appPrefix) && !archivePath.startsWith(driverSourcePrefix)) return false;
  if (redistributedDriverFiles.includes(archivePath)) return false;
  const extension = path.posix.extname(archivePath).slice(1).toLowerCase();
  return omittedExtensions.includes(extension) || (extension === 'zip' && archivePath.startsWith(driverPackagePrefix));
}
