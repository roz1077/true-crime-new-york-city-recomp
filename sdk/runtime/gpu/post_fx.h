#pragma once
// Full-screen passes on the finished frame that both renderers share: the bloom chain. TCNYC_VULKAN is 1 in the
// Vulkan shader compiler and 0 on D3D12; the constants (a cbuffer at b1 there, push constants here) and the
// full-screen triangle's NDC direction are the only splits. Textures t0 and t1 are bindings 2 and 3 on Vulkan,
// samplers s0 (linear, clamped) and s1 (point) bindings 6 and 7.

#include <string>

#include "../../third_party/fsr1/FsrShader.h"

namespace gpu {

// The bloom chain, built before the final blit from the image it reads. bright: the bright pass the blit used to
// run per pixel (a 2x2 box, the gamma ramp, everything above 0.94), at half size. down: a 13-tap filter to the next
// smaller level. up: the larger level's own image times its weight, plus a 3x3 tent of the smaller finished level.
// The weights add up to 1, so the finished half-size level has the brightness of the old 9-tap star, spread
// smoothly instead of in eight directions.
// a: xy the texel size of the image filtered (t0 for bright and down, t1 for up), z the gamma flag (bright) or this
// level's weight (up), w the weight of the smaller level (up; the smallest level is not weighted until then)
inline const char* kBloomHlsl = R"(
#if TCNYC_VULKAN
struct PostPC { float4 a; float4 unused; };
[[vk::push_constant]] ConstantBuffer<PostPC> postPC;
#define A postPC.a
#else
cbuffer B : register(b1) { float4 a_; float4 unused_; };
#define A a_
#endif
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
#if TCNYC_VULKAN
    o.p = float4(t * float2(2, 2) + float2(-1, -1), 0, 1);
#else
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
#endif
    o.uv = t;
    return o;
}
float3 S0(float2 uv) { return t0.SampleLevel(ls, uv, 0).rgb; }
float4 bright(O i) : SV_Target {
    float2 q = A.xy * 0.5;
    float3 c = (S0(i.uv + float2(-q.x, -q.y)) + S0(i.uv + float2(q.x, -q.y)) +
                S0(i.uv + float2(-q.x, q.y)) + S0(i.uv + float2(q.x, q.y))) * 0.25;
    if (A.z > 0) c = float3(t1.SampleLevel(ps, float2(c.r, 0.5), 0).r, t1.SampleLevel(ps, float2(c.g, 0.5), 0).g,
                            t1.SampleLevel(ps, float2(c.b, 0.5), 0).b);
    return float4(max(c - 0.94, 0.0), 1);
}
float4 down(O i) : SV_Target {
    float2 d = A.xy;
    float3 e = S0(i.uv);
    float3 corners = S0(i.uv + d * float2(-2, -2)) + S0(i.uv + d * float2(2, -2)) +
                     S0(i.uv + d * float2(-2, 2)) + S0(i.uv + d * float2(2, 2));
    float3 edges = S0(i.uv + d * float2(0, -2)) + S0(i.uv + d * float2(-2, 0)) +
                   S0(i.uv + d * float2(2, 0)) + S0(i.uv + d * float2(0, 2));
    float3 inner = S0(i.uv + d * float2(-1, -1)) + S0(i.uv + d * float2(1, -1)) +
                   S0(i.uv + d * float2(-1, 1)) + S0(i.uv + d * float2(1, 1));
    return float4(e * 0.125 + corners * 0.03125 + edges * 0.0625 + inner * 0.125, 1);
}
float4 up(O i) : SV_Target {
    float2 d = A.xy;
    float3 s = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
            s += t1.SampleLevel(ls, i.uv + d * float2(x, y), 0).rgb * ((2 - abs(x)) * (2 - abs(y)));
    return float4(S0(i.uv) * A.z + s * (A.w / 16.0), 1);
}
)";

// level weights, half size first; they add up to 1
inline constexpr unsigned kBloomLevels = 4;
inline constexpr float kBloomWeights[kBloomLevels] = {0.10f, 0.30f, 0.35f, 0.25f};

// AMD FidelityFX FSR 1 as two full-screen pixel shaders: easu upscales t0 to the target's size, rcas sharpens t0
// into a target of the same size. Both take the source image in the display's gamma, as the blit reads it.
// a: xy the source size in pixels, zw the target size; b.x RCAS sharpness in stops (0 the strongest).
// The constants FsrEasuCon and FsrRcasCon build are made per pixel from these, as AMD's headers allow.
inline const std::string& FsrHlsl() {
    static const std::string source = std::string(R"(
#if TCNYC_VULKAN
struct PostPC { float4 a; float4 b; };
[[vk::push_constant]] ConstantBuffer<PostPC> postPC;
#define FSR_PA postPC.a
#define FSR_PB postPC.b
#else
cbuffer B : register(b1) { float4 a_; float4 b_; };
#define FSR_PA a_
#define FSR_PB b_
#endif
Texture2D t0 : register(t0);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
#if TCNYC_VULKAN
    o.p = float4(t * float2(2, 2) + float2(-1, -1), 0, 1);
#else
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
#endif
    o.uv = t;
    return o;
}
#define A_GPU 1
#define A_HLSL 1
)") + kFfxAHlsl + R"(
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return t0.GatherRed(ls, p); }
AF4 FsrEasuGF(AF2 p) { return t0.GatherGreen(ls, p); }
AF4 FsrEasuBF(AF2 p) { return t0.GatherBlue(ls, p); }
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return t0.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)" + kFfxFsr1Hlsl + R"(
float4 easu(O i) : SV_Target {
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, FSR_PA.x, FSR_PA.y, FSR_PA.x, FSR_PA.y, FSR_PA.z, FSR_PA.w);
    AF3 c;
    FsrEasuF(c, AU2(i.p.xy), con0, con1, con2, con3);
    return float4(c, 1);
}
float4 rcas(O i) : SV_Target {
    AU4 con;
    FsrRcasCon(con, FSR_PB.x);
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(i.p.xy), con);
    return float4(r, g, b, 1);
}
)";
    return source;
}

// The Sharpening setting (percent, 1 to 100) as RCAS stops: 100% is RCAS at full strength (0 stops), each 50% less
// halves it. FSR upscaling without the setting uses AMD's suggested 0.2 stops.
inline float RcasStops(unsigned percent) { return percent ? float(100 - (percent > 100 ? 100 : percent)) / 50.0f : 0.2f; }

}  // namespace gpu
