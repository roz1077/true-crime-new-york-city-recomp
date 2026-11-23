#include <cmath>
#include <cstdio>

#include "xdk/dsound_mix.h"

namespace {

int g_failures = 0;

void ExpectNear(float actual, float expected, float eps, const char* label) {
    if (std::fabs(actual - expected) > eps) {
        std::fprintf(stderr, "FAIL %s: got %.6f expected %.6f (eps %.6f)\n", label, actual, expected, eps);
        g_failures++;
    }
}

void ExpectTrue(bool cond, const char* label) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", label);
        g_failures++;
    }
}

// The stereo fold Render applies to the summed speaker and crosstalk bins of every channel.
float FoldLeft(const float (&bin)[10][32], int channels) {
    float sum = 0.0f;
    for (int c = 0; c < channels; c++)
        sum += bin[c][0] + bin[c][6] + dsound_mix::kFold * (bin[c][2] + bin[c][4] + bin[c][8]);
    return sum;
}

float FoldRight(const float (&bin)[10][32], int channels) {
    float sum = 0.0f;
    for (int c = 0; c < channels; c++)
        sum += bin[c][1] + bin[c][7] + dsound_mix::kFold * (bin[c][2] + bin[c][5] + bin[c][9]);
    return sum;
}

void TestStreamHeadroom() {
    // 2D streams cap headroom at 300 mB (-3 dB) so the game's 900 mB pool does not bury cutscenes.
    ExpectNear(dsound_mix::StreamHeadroomGain(false, 900), dsound_mix::Db(-300), 1e-5f, "2d_900mb_capped");
    ExpectNear(dsound_mix::StreamHeadroomGain(false, 600), dsound_mix::Db(-300), 1e-5f, "2d_600mb_capped");
    ExpectNear(dsound_mix::StreamHeadroomGain(false, 200), dsound_mix::Db(-200), 1e-5f, "2d_200mb_preserved");
    ExpectNear(dsound_mix::StreamHeadroomGain(false, 0), 1.0f, 1e-5f, "2d_0mb");
    // 3D streams keep their requested headroom unmodified.
    ExpectNear(dsound_mix::StreamHeadroomGain(true, 900), dsound_mix::Db(-900), 1e-5f, "3d_900mb_preserved");
    ExpectNear(dsound_mix::StreamHeadroomGain(true, 0), 1.0f, 1e-5f, "3d_0mb");
}

void TestNormalize2DFold() {
    // Normal 2ch stereo stream on FL/FR stays untouched.
    {
        float bin[10][32] = {};
        bin[0][0] = 0.5f;
        bin[1][1] = 0.5f;
        dsound_mix::Normalize2DFold(bin, 2);
        ExpectNear(bin[0][0], 0.5f, 1e-5f, "stereo_2bin_fl");
        ExpectNear(bin[1][1], 0.5f, 1e-5f, "stereo_2bin_fr");
    }
    // Panned 2ch stereo voice stays untouched.
    {
        float bin[10][32] = {};
        bin[0][0] = 0.8f;
        bin[1][1] = 0.2f;
        dsound_mix::Normalize2DFold(bin, 2);
        ExpectNear(bin[0][0], 0.8f, 1e-5f, "panned_fl");
        ExpectNear(bin[1][1], 0.2f, 1e-5f, "panned_fr");
    }
    // Bink 2ch voice sent to all its speaker bins: left to FL, centre and back left, right to FR, centre
    // and back right (bin 3 is the sub, which the fold drops). Its stereo fold comes back to unity.
    {
        float bin[10][32] = {};
        bin[0][0] = 0.5f;
        bin[0][2] = 0.5f;
        bin[0][4] = 0.5f;
        bin[1][1] = 0.5f;
        bin[1][2] = 0.5f;
        bin[1][5] = 0.5f;
        dsound_mix::Normalize2DFold(bin, 2);
        ExpectNear(FoldLeft(bin, 2), 0.5f, 1e-5f, "bink_6bin_left_unity");
        ExpectNear(FoldRight(bin, 2), 0.5f, 1e-5f, "bink_6bin_right_unity");
    }
    // Mono voice on FL, FR and centre: the centre adds to both sides, so the fold comes back to unity.
    {
        float bin[10][32] = {};
        bin[0][0] = 0.5f;
        bin[0][1] = 0.5f;
        bin[0][2] = 0.5f;
        dsound_mix::Normalize2DFold(bin, 1);
        ExpectNear(FoldLeft(bin, 1), 0.5f, 1e-5f, "mono_lrc_left_unity");
        ExpectNear(FoldRight(bin, 1), 0.5f, 1e-5f, "mono_lrc_right_unity");
    }
    // The sub is not part of the stereo fold: a voice on FL, FR and the sub stays untouched.
    {
        float bin[10][32] = {};
        bin[0][0] = 0.5f;
        bin[1][1] = 0.5f;
        bin[1][3] = 0.5f;
        dsound_mix::Normalize2DFold(bin, 2);
        ExpectNear(bin[0][0], 0.5f, 1e-5f, "sub_ignored_fl");
        ExpectNear(bin[1][1], 0.5f, 1e-5f, "sub_ignored_fr");
    }
}

void TestOutputMakeupAndSoftLimit() {
    ExpectNear(dsound_mix::OutputMakeupGain(false, 1, 1), 1.0f, 1e-5f, "makeup_no_dsp");
    ExpectNear(dsound_mix::OutputMakeupGain(true, 1, 1), 2.0f, 1e-5f, "makeup_dsp_default");
    ExpectNear(dsound_mix::OutputMakeupGain(true, 0, 1), 1.0f, 1e-5f, "makeup_dsp_zero_headroom");

    // Transparent below knee (0.90), bounded in (-1, 1) above knee.
    ExpectNear(dsound_mix::SoftLimit(0.5f), 0.5f, 1e-6f, "softlimit_linear_pos");
    ExpectNear(dsound_mix::SoftLimit(-0.90f), -0.90f, 1e-6f, "softlimit_knee_neg");
    const float warm = dsound_mix::SoftLimit(1.0f);
    ExpectTrue(warm > 0.90f && warm < 1.0f, "softlimit_warm_bounded");
    const float hot = dsound_mix::SoftLimit(2.0f);
    ExpectTrue(hot > 0.99f && hot <= 1.0f, "softlimit_hot_bounded");
    ExpectNear(dsound_mix::SoftLimit(-2.0f), -hot, 1e-6f, "softlimit_symmetric");
}

}  // namespace

int main() {
    TestStreamHeadroom();
    TestNormalize2DFold();
    TestOutputMakeupAndSoftLimit();
    return g_failures ? 1 : 0;
}
