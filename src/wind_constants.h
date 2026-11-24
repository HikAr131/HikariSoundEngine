// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Every tuned value of the wind stage. tests/delta-golden.json repeats them, so a change has to be made twice on purpose.
namespace hikari::wind {
constexpr double kRate = 48000;
constexpr std::size_t kBlock = 384;
constexpr std::size_t kWindow = 48000;

// Fingerprint
constexpr std::size_t kSubFrame = 960;
constexpr std::size_t kSubFrames = 50;
constexpr std::size_t kDecimation = 6;
constexpr std::size_t kTaps = 160;
constexpr double kTukeyAlpha = 0.25;
constexpr std::size_t kFingerprintFft = 256;
constexpr std::size_t kBands = 16;
constexpr double kBandLowHz = 100;
constexpr double kBandSpan = 32;
constexpr double kPowerFloor = 1e-30;
constexpr double kNormFloor = 1e-12;
constexpr std::size_t kCoarseDecimation = 16;
constexpr std::size_t kRawTailStart = 43904;
constexpr std::size_t kRawTail = 4096;

// Presence
constexpr double kPresencePower = 2.5e-6;
constexpr double kPresenceCosine = 0.97;

// Search
constexpr std::size_t kSearchInterval = 24000;
constexpr std::size_t kMaxPeaks = 4;
constexpr std::size_t kPeakDistance = 2880;
constexpr std::size_t kCoarseRadius = 480;
constexpr std::size_t kCoarseLags = 61;
constexpr std::size_t kFineRadius = 12;
constexpr double kTryTwoScore = 0.4;
constexpr double kAlignScore = 0.35;

// Alignment
constexpr double kLooseCorrelation = 0.2;
constexpr std::size_t kWholeRadius = 2;
constexpr std::size_t kSegment = 8192;
constexpr std::size_t kSegmentRadius = 24;
constexpr std::size_t kSegmentSpan = 39808;
constexpr double kMaxRateError = 0.0002;
constexpr double kMonoRatio = 1e-8;
constexpr double kMonoFloor = 1e-20;
constexpr std::int64_t kRecentLockFrames = 288000;
// Sound heard this long without any wind in it is told apart from a search that has only begun.
constexpr std::int64_t kFruitlessFrames = 1440000;
constexpr double kHeardPower = 1e-8;
constexpr double kRecentLockDistance = 8;
constexpr std::size_t kMaxSlots = 4;
constexpr double kHandoverRadius = 24;
constexpr double kHandoverPerFrame = 0.0004;
constexpr std::size_t kLocalBack = 28800;
constexpr std::size_t kLocalPoints = 1200;

// Confirmation
constexpr double kConfirmLow = 0.038518362;
constexpr double kConfirmHigh = 0.407615152;
constexpr std::size_t kConfirmFrames = 15360;
constexpr std::size_t kConfirmWarmup = 768;
constexpr std::array<int, 5> kConfirmLags = {8, 16, 24, 32, 40};
constexpr double kConfirmCorrelation = 0.25;
constexpr double kConfirmMargin = 0.003;
constexpr int kConfirmPasses = 2;

// Tracking
constexpr int kTrackRadius = 12;
constexpr std::size_t kTrackFrames = 4096;
constexpr std::size_t kTrackEveryBlocks = 4;
constexpr std::size_t kTrackFineSteps = 16;
constexpr std::size_t kSincTaps = 32;
constexpr std::size_t kSincPhases = 1024;
constexpr double kSincBeta = 8.6;
constexpr double kTrackPeak = 0.9;
constexpr double kTrackSidelobe = 0.05;
constexpr double kJumpOffset = 0.5;
constexpr double kResetOffset = 0.25;
constexpr std::int64_t kRealignFrames = 192000;

// Model
constexpr std::size_t kSpectrumFft = 1024;
constexpr std::size_t kSpectrumHop = 256;
constexpr std::size_t kSpectrumFrames = 21;
constexpr std::size_t kCutoffs = 101;
constexpr double kCutoffLowHz = 500;
constexpr double kCutoffSpan = 46;
constexpr double kNoCutoffHz = 24000;
constexpr double kBypassCutoffHz = 23999;
constexpr double kSnapCutoffHz = 23990;
constexpr std::array<double, 9> kFitEdges = {20, 250, 500, 1500, 3250, 6500, 10000, 16000, 20001};
constexpr double kFitCoherence = 0.65;
constexpr double kFitPowerRatio = 1e-9;
constexpr std::size_t kFitMinBins = 2;
constexpr double kFitBandCoherence = 0.75;
constexpr double kFitMinMagnitude = 1e-8;
constexpr double kFitMaxWeight = 20;
constexpr double kFitWeightFloor = 0.02;
constexpr std::size_t kFitMinBands = 4;
constexpr double kFitNoCutoffMargin = 0.0025;
constexpr double kFitMinGain = 1e-5;
constexpr double kFitMaxGain = 2;
constexpr double kFitMaxError = 0.099856;
constexpr double kSmoothing = 0.16;
constexpr double kGainDownDb = 0.64;
constexpr double kGainUpDb = 0.8;
constexpr double kCutoffStepOctaves = 0.10666666666666666;
constexpr std::size_t kPresenceRing = 32;
constexpr double kPresenceEffective = 0.25;
constexpr double kPresenceLow = 0.09;
constexpr double kPresenceHigh = 0.36;
constexpr double kPresenceDown = 0.016;
constexpr double kPresenceUp = 0.16;

// Cancellation
constexpr std::array<double, 3> kCrossovers = {500, 3250, 6500};
constexpr std::size_t kEnergyBlocks = 8;
constexpr double kLouderLimitDb = 1.5;
constexpr double kMismatchPower = 3.84e-10;
constexpr double kMismatchRatio = 0.1;
constexpr int kMismatchBands = 2;
constexpr double kRecoverStep = 1.0 / 38;
constexpr double kFadeInSeconds = 0.5;
constexpr double kFadeOutSeconds = 0.3;
constexpr double kSwitchSeconds = 0.01;

// Quality
constexpr double kQualityLowHz = 100;
constexpr double kQualityHighHz = 3200;
constexpr double kQualityFloor = 1e-24;
constexpr double kQualityBad = 0.1;
constexpr std::size_t kQualityBlocks = 188;
constexpr std::size_t kQualityMaxBad = 124;
constexpr double kQualityDrop = 0.25;
constexpr double kQualityReturn = 0.6;
constexpr std::int64_t kForgetFrames = 2880000;

// Eighth-order band-pass in front of the delay estimate, roughly 4-15 kHz at 48 kHz.
struct Section { double b0, b1, b2, a1, a2; };
constexpr std::array<Section, 4> kTrackFilter = {{
    {0.09398085143379442, 0.18796170286758884, 0.09398085143379442, 0.29808827664000576, 0.12197872109365555},
    {1, -2, 1, -1.0301390842088836, 0.32436911570995297},
    {1, 2, 1, 0.7673189938478382, 0.599256573550772},
    {1, -2, 1, -1.4993698014167163, 0.7450276090027126},
}};
} // namespace hikari::wind
