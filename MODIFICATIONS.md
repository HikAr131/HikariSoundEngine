# Upstream modifications

Hikari modifications dated 2026-10-04 are applied to a build copy of FxSound app at
`d8e7a23d37ed5939c2a3090a1c1756c7f2500b17`. The pinned submodule remains unchanged.
Every modified upstream file begins with a dated modification statement.

`patches/01-headless-host.patch` changes only the following integration boundaries:

| Files | Modification and purpose |
| --- | --- |
| `dsp/DfxDspPrivate.cpp`, `audiopassthru/src/sndDevices/sndDevicesReg.cpp` | Product registry paths are rooted at `HKCU\SOFTWARE\Hikari1U\SoundEngine`, retaining the upstream version/vendor suffixes. DSP initialization errors become C++ errors handled by the host. The preset-list handle is initialized to NULL alongside the existing handle initialization. |
| Both `codedefs.h`, `operatingSystem.cpp` | Error diagnostics use the host logging hook instead of modal windows. |
| `sndDevicesInit.cpp` | Removes the unused `BladeMP3EncDLL.h` include. MP3 encoding is not compiled or distributed. |
| `AudioPassthruPrivate.cpp` | Replaces the DSP format/process calls with a host hook that serializes format, parameters and processing and prepends the compatible parametric EQ. The capture/playback loop and DSP mathematics are unchanged. |
| `sndDevicesSet.cpp` | Every upstream `SetDefaultEndpoint` call passes the host permission gate, including restoration. |
| `sndDevicesDeviceCallbacks.cpp` | Reports default-device changes to the host before the upstream early returns, allowing conflict accounting and yielding. |
| `sndDevicesImplementDeviceRules.cpp` | Uses the host's explicitly selected physical output after upstream selection. A missing requested endpoint fails closed, allowing fixed and debug output selection without an intermediate system default change. |
| `sndDevicesSetupDevices.cpp` | Validates float32 sample representation and supported channel layout before caching either endpoint format. Every capture/render `Initialize` call validates the actual fresh mix or closest-match format against the cached format; a mismatch becomes `AUDCLNT_E_UNSUPPORTED_FORMAT` before buffer initialization. The final playback `Initialize` HRESULT, success or failure, is reported to the host through `hikariOnPlaybackInitializeResult` immediately before the upstream failure branch; the upstream retry and park logic is unchanged. |
| Three registry implementation files under `audiopassthru/src/reg/` | Redirect registry calls through a host boundary; upstream writes outside the own HKCU root and all upstream deletes are rejected. Offline tests route all own-root reads/writes into a volatile temporary key under HKCU Software and remove it afterward. |

The build compiles only the source lists in the pinned `audiopassthru.vcxproj` and
`DfxDsp.vcxproj`, into static libraries, using x64 MSVC v143 and `/MT`.
GUI, JUCE, installer and auxiliary products are excluded.
Per the 2026-10-04 plan revision, the upstream graphic equalizer is always
disabled. Both graphic and parametric equalization run in the Hikari segment
before the upstream effects and always-active optimizer/limiter. No upstream
DSP mathematics or capture/playback-loop code is changed.
The upstream five-effect public setter takes a value in 0–10 and divides it by
10 before passing it to the SDK's 0–1 range and caching it. The public getter
returns that 0–1 cached value. Hikari passes protocol values directly, matching
the upstream controller's 0–10 setter path, and checks normalized readback.

Offline construction, processing and destruction exposed an uninitialized
`preset_list_handle_` in the pinned upstream constructor. All 96 stereo sine
blocks completed successfully before destruction raised access violation
`0xC0000005` at `prelstFreeUp+0x15`, called by `DfxDspPrivate::~DfxDspPrivate`.
The constructor now initializes that handle to NULL. The existing destructor,
audio loop and DSP mathematics remain unchanged.

The pinned `dsp/ptechDsp/Maximizer/Maxi32/Maxi32.c` sets `max_output` to
`0.966051` (line 91) and multiplies the input by it (lines 297 and 391), even
at zero dynamic boost. A low-level five-frequency sine comparison confirmed
a constant nominal attenuation of approximately 0.3 dB. The Hikari segment
calibrates the input to that unchanged optimizer by `1 / 0.966051`, after EQ
and before upstream processing, to meet the zero-effects flat-response target.
The calibration applies only to the processed path; the optimizer remains
active, and full-scale multichannel tests still require finite output at or
below unity.
The allowed 128-filter extreme (128 enabled PK filters at 1000 Hz, +20 dB,
Q 10, with +20 dB preamp) produced NaN inside upstream processing on the
second stereo block even at 0.001 input level. The host input calibration
therefore also replaces non-finite values with zero and bounds the signal
entering upstream to ±16 (24 dB above full scale). This boundary retains the
headroom exercised by the graphic maximum test while preventing the enormous
cascade values from overflowing upstream's single-precision calculations.
The upstream limiter still determines the final output; the extreme test
requires finite output no greater than unity at both low and full input levels
for 2, 4, 6 and 8 channels.

The format boundary accepts stereo float (legacy without a mask or extensible
mask `0x3`), true Quad (`0x33`), 5.1 back/side (`0x3f`/`0x60f`, whose six
positional samples retain the same upstream order), and standard 7.1 (`0x63f`).
All formats require 32-bit float, 32 valid bits where extensible, consistent
block alignment and byte rate, and a sample rate from 22050 through 192000 Hz.
Legacy multichannel formats, ambiguous/mismatched masks, and PCM are rejected.
The second mix-format query and both closest-match initialization paths must
retain every cached WAVEFORMATEX field, preventing a format change from leaving
the buffer-processing loop with a stale channel count or byte layout.

The pin includes the upstream 1.2.15 development line, including recent device
enumeration and shutdown work. Latency, drift, final-output disconnect and sleep
requests require a virtual machine or test hardware; offline DSP success does
not establish those results.

The signed `Version14/win10/x64` driver files are copied unchanged. The build does
not install drivers, alter default devices, play audio or create startup entries.

Offline measure isolation uses a fresh GUID in its volatile HKCU test key, in
addition to the PID. A reused PID cannot collide with a key left by an earlier
abnormal test exit. The process only deletes the exact key it created.

Host-side handling added in 1.0.1 (2026-10-04) changes no upstream code. The
host no longer toggles the upstream power switch for bypass: it renders bypass
as a 20 ms fade between the processed output and the untouched input, so the
upstream effects and optimizer keep running underneath and leaving bypass
fades back into a settled state. Upstream effect setters are called only when
a value changes, and the constant settings (power on, upstream EQ off, balance,
normalization, volume leveling, master gain) are applied once per DSP
instance; the setters stay serialized with processing because the upstream
DSP is not thread-safe. The playback `Initialize` report described in the
table above is the only new patch hook.
