// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "audio_format.h"

namespace hikari {
bool validateAudioFormat(const WAVEFORMATEX* format) noexcept {
    if (!format || format->wBitsPerSample != 32 || format->nSamplesPerSec < 22050 ||
        format->nSamplesPerSec > 192000 || format->nBlockAlign != format->nChannels * sizeof(float) ||
        format->nAvgBytesPerSec != format->nSamplesPerSec * format->nBlockAlign) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return format->nChannels == 2 && format->cbSize == 0;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
    const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    constexpr GUID floatSubFormat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    if (extended->Samples.wValidBitsPerSample != 32 ||
        !IsEqualGUID(extended->SubFormat, floatSubFormat)) return false;
    const DWORD mask = extended->dwChannelMask;
    switch (format->nChannels) {
    case 2: return mask == 0x3;
    case 4: return mask == 0x33;
    case 6: return mask == 0x3f || mask == 0x60f;
    case 8: return mask == 0x63f;
    default: return false;
    }
}
bool matchesCachedAudioFormat(const WAVEFORMATEX* format, const WAVEFORMATEX& cached) noexcept {
    return validateAudioFormat(format) && format->wFormatTag == cached.wFormatTag &&
        format->nChannels == cached.nChannels && format->nSamplesPerSec == cached.nSamplesPerSec &&
        format->nAvgBytesPerSec == cached.nAvgBytesPerSec && format->nBlockAlign == cached.nBlockAlign &&
        format->wBitsPerSample == cached.wBitsPerSample && format->cbSize == cached.cbSize;
}
}
bool hikariValidateAudioInitialization(const WAVEFORMATEX* format, const WAVEFORMATEX* cached) noexcept {
    return cached && hikari::matchesCachedAudioFormat(format, *cached);
}
