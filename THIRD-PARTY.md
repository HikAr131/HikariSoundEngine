# Third-party notices

HikariSoundEngine host code: Copyright (C) 2026 Hikari, AGPL-3.0-or-later.

FxSound application audio libraries: Copyright (C) 2025 FxSound LLC and the contributors named in each source file. Source: https://github.com/fxsound2/fxsound-app, commit `d8e7a23d37ed5939c2a3090a1c1756c7f2500b17`. Repository LICENSE is AGPL version 3; file notices permit version 3 or later. Only `audiopassthru` and `dsp` are compiled. Upstream notices remain in the build copies. Local changes are listed in MODIFICATIONS.md and each modified file carries a dated notice.

FxSound virtual audio driver: https://github.com/fxsound2/fxsound-driver, source reference `c78fc6d031d16bd0a5dbbdff4871cfb8715d343d`. Repository license is AGPL v3, with Microsoft Limited Public License version 1.1 for the Microsoft sample-derived code. LICENSE-MS-LPL.rtf preserves the original upstream notice, LICENSE-MS-LPL.txt contains its text. The driver is derived from Microsoft's MSVAD sample. The copied `fxvad.inf`, `fxvad.sys`, and `fxvadntamd64.cat` are unmodified files from the app pin's `Installer/Drivers/Version14/win10/x64/`; no driver installer is compiled into this helper. Driver version is 14.1.0.0. A source-to-binary correspondence has not been independently established; these source references are not a claim of reproducibility for the signed driver.

`audiopassthru/include/PolicyConfig.h`: upstream credits EreTIk and points to the historical soundprison project on Google Code. This declares undocumented Windows COM interfaces. No separate license notice appears in that header; it is retained under the upstream repository's license, with this provenance uncertainty recorded.

Unresolved upstream provenance:

- `dsp/ptutil/DspUtil/BinauralSync/IRC_1057_*.h`: HRTF coefficient tables have no individual license notice in the source. They are inherited from the AGPL repository; their independent provenance is unresolved.
- Legacy DSP headers, including `dsp/ptutil/include/mth.h`, retain Power Technology confidentiality statements inconsistent with the repository's AGPL license. Those statements have been preserved; repository licensing does not independently resolve their history.

`BladeMP3EncDLL.h` is excluded from compilation. GUI, JUCE, installers, fxmcp and fxdiag are excluded. No unrelated restricted source code is used.

FxSound is a trademark of FxSound LLC. HikariSoundEngine is an independent derivative and is not an official FxSound product. The virtual device keeps its FxSound name because the signed driver is unmodified. No FxSound product icon or branding is used as the helper's identity.

Redistribution remains subject to the listed licenses. No signed release or public source repository has been published by this local implementation session.
