// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "audio_format.h"
#include <initializer_list>
#include <stdexcept>

namespace hikari {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
WAVEFORMATEXTENSIBLE makeFormat(WORD channels, DWORD mask) {
    WAVEFORMATEXTENSIBLE result{};
    result.Format = {WAVE_FORMAT_EXTENSIBLE, channels, 48000, static_cast<DWORD>(48000 * channels * 4),
        static_cast<WORD>(channels * 4), 32, 22};
    result.Samples.wValidBitsPerSample = 32;
    result.dwChannelMask = mask;
    result.SubFormat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    return result;
}
}
void runAudioFormatTests() {
    for (const auto& format : {makeFormat(2, 0x3), makeFormat(4, 0x33),
        makeFormat(6, 0x3f), makeFormat(6, 0x60f), makeFormat(8, 0x63f)}) {
        require(validateAudioFormat(&format.Format), "Supported float layout rejected");
        const WAVEFORMATEX cached = format.Format;
        require(hikariValidateAudioInitialization(&format.Format, &cached), "Matching WASAPI format rejected");
    }
    WAVEFORMATEX stereo{WAVE_FORMAT_IEEE_FLOAT, 2, 44100, 44100 * 8, 8, 32, 0};
    require(validateAudioFormat(&stereo), "Legacy stereo float rejected");
    require(!validateAudioFormat(nullptr), "Null WASAPI format accepted");
    require(!hikariValidateAudioInitialization(&stereo, nullptr), "Null cached format accepted");
    for (const auto& format : {makeFormat(4, 0xf), makeFormat(4, 0x603), makeFormat(8, 0x6cf),
        makeFormat(6, 0), makeFormat(2, 0), makeFormat(3, 0x7), makeFormat(5, 0x37), makeFormat(7, 0x13f)})
        require(!validateAudioFormat(&format.Format), "Unsupported WASAPI layout accepted");
    auto valid = makeFormat(4, 0x33);
    auto bad = valid; bad.SubFormat.Data1 = 1;
    require(!validateAudioFormat(&bad.Format), "PCM format accepted as float");
    bad = valid; bad.Samples.wValidBitsPerSample = 24;
    require(!validateAudioFormat(&bad.Format), "Non-32 valid bits accepted");
    bad = valid; bad.Format.cbSize = 20;
    require(!validateAudioFormat(&bad.Format), "Truncated extensible format accepted");
    bad = valid; bad.Format.wBitsPerSample = 16;
    require(!validateAudioFormat(&bad.Format), "Non-32 storage bits accepted");
    bad = valid; bad.Format.nBlockAlign = 8;
    require(!validateAudioFormat(&bad.Format), "Invalid block alignment accepted");
    bad = valid; bad.Format.nAvgBytesPerSec -= 1;
    require(!validateAudioFormat(&bad.Format), "Invalid average byte rate accepted");
    bad = valid; bad.Format.nSamplesPerSec = 0; bad.Format.nAvgBytesPerSec = 0;
    require(!validateAudioFormat(&bad.Format), "Invalid sample rate accepted");
    const WAVEFORMATEX cached = valid.Format;
    auto changed = makeFormat(2, 0x3);
    require(!hikariValidateAudioInitialization(&changed.Format, &cached), "Changed render channel count accepted");
    changed = valid; changed.Format.nSamplesPerSec = 44100; changed.Format.nAvgBytesPerSec = 44100 * 16;
    require(validateAudioFormat(&changed.Format), "Valid changed rate rejected as unsupported");
    require(!hikariValidateAudioInitialization(&changed.Format, &cached), "Changed render rate accepted");
    changed = valid; changed.Format.cbSize = 24;
    require(!hikariValidateAudioInitialization(&changed.Format, &cached), "Changed format metadata accepted");
    stereo.nChannels = 4; stereo.nBlockAlign = 16; stereo.nAvgBytesPerSec = 44100 * 16;
    require(!validateAudioFormat(&stereo), "Legacy multichannel format accepted without layout");
}
}
