#pragma once
// Host DirectSound mixer gain-staging helpers (pure math, tested by host unit tests).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dsound_mix {

// The console's stereo downmix of the speaker bins: centre and back pair at -3 dB, the sub dropped.
inline constexpr float kFold = 0.707f;

// The title's 2D stereo stream pool (cutscene WMA and streamed music) calls SetHeadroom(900)
// (-9 dB) while 3D voice streams and 2D SFX use 0 mB. Capping non-3D stream headroom at
// 300 mB (-3 dB) keeps cutscene voices and music within 3 dB of gameplay voices.
inline constexpr int32_t kMax2DStreamHeadroomMb = 300;

inline float Db(int32_t hundredths) {
    return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f);
}

inline float StreamHeadroomGain(bool is3d, int32_t mb) {
    if (mb < 0) mb = 0;
    if (!is3d && mb > kMax2DStreamHeadroomMb) mb = kMax2DStreamHeadroomMb;
    return Db(-mb);
}

// A 2D voice that duplicates its channels across front, centre, and surround bins (such as
// Bink video sending 2ch to all 6 speaker bins) sums to (1 + 2 * kFold) = 2.414x (+7.65 dB)
// when folded to stereo. Scale its speaker/crosstalk bins so its stereo fold stays at unity
// relative to its loudest single bin, leaving normal 2-bin stereo/mono 2D voices untouched.
template <size_t Rows, size_t Bins>
inline void Normalize2DFold(float (&bin)[Rows][Bins], uint32_t channels) {
    constexpr uint32_t kFL = 0, kFR = 1, kC = 2, kSub = 3, kBL = 4, kBR = 5;
    constexpr uint32_t kXFL = 6, kXFR = 7, kXBL = 8, kXBR = 9, kFirstSend = 10;
    const uint32_t nch = std::min<uint32_t>(channels, uint32_t(Rows));
    float peak_bin = 0.0f;
    float left_fold = 0.0f;
    float right_fold = 0.0f;
    for (uint32_t c = 0; c < nch; c++) {
        for (uint32_t b = 0; b < kFirstSend && b < Bins; b++) {
            if (b == kSub) continue;
            peak_bin = std::max(peak_bin, bin[c][b]);
        }
        left_fold += bin[c][kFL] + bin[c][kXFL] + kFold * (bin[c][kC] + bin[c][kBL] + bin[c][kXBL]);
        right_fold += bin[c][kFR] + bin[c][kXFR] + kFold * (bin[c][kC] + bin[c][kBR] + bin[c][kXBR]);
    }
    const float peak_fold = std::max(left_fold, right_fold);
    if (peak_bin > 0.0f && peak_fold > peak_bin) {
        const float scale = peak_bin / peak_fold;
        for (uint32_t c = 0; c < nch; c++) {
            for (uint32_t b = 0; b < kFirstSend && b < Bins; b++) bin[c][b] *= scale;
        }
    }
}

// When the DSP effects image is loaded, SlotGains shifts every bin down by 2^headroom
// (default 1 = -6 dB) to prevent DSP intermediate saturation, and the standard effects
// image does not restore that shift on the front speaker bins. Restore it at output.
inline float OutputMakeupGain(bool dsp_loaded, uint8_t front_left_headroom, uint8_t front_right_headroom) {
    if (!dsp_loaded) return 1.0f;
    return std::ldexp(1.0f, std::min(front_left_headroom, front_right_headroom));
}

// Transparent up to 0.90 (-0.9 dBFS), then smoothly saturates into [-1, 1] so loud
// multi-voice transients never harsh-clip after makeup gain.
inline float SoftLimit(float x) {
    constexpr float kKnee = 0.90f;
    const float a = std::fabs(x);
    if (a <= kKnee) return x;
    const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
    return std::copysign(y, x);
}

}  // namespace dsound_mix
