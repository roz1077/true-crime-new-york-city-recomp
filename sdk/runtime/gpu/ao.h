#pragma once

#include <algorithm>
#include <cstdint>
#include "core/dev_env.h"
#include <cstdlib>
#include <string>

#include "gpu/state.h"

namespace gpu {

// Scene AO classification. World geometry blends one of three ways: not at all, ONE/ZERO (no
// blend in effect; most of the world), or as an alpha surface (SRCALPHA/INVSRCALPHA, add).
// Alpha-tested geometry (trees, fences, railings) is drawn twice: a depth-only pass that writes
// depth and no colour, then an alpha colour pass that tests depth but does not write it.
// Additive, multiply and other blends are light shafts, gobos, fog sheets, reflections and
// glows. Some write depth; they must stay out of the AO depth, because AO computed on a flat
// effect quad is 1, which left the wall behind it undarkened (the white bands).
enum AoRole : uint8_t { kAoDepth = 1, kAoColour = 2 };

constexpr float kAoDistClear = 1e30f;  // no geometry: the sky, or only effects

inline float AoEnvFloat(const char* name, float fallback) {
    const char* e = DevEnv(name);
    return e && *e ? std::strtof(e, nullptr) : fallback;
}

// Both are experiments that did not remove the band-edge artifact on flat walls; off by default.
// TCNYC_AO_THIN=1 turns on the isolated-depth-sample rejection.
inline bool AoThinSamples() {
    const char* e = DevEnv("TCNYC_AO_THIN");
    return e && *e == '1';
}

// TCNYC_AO_FLAT=1 turns on the derivative normal used on flat surfaces.
inline bool AoFlatNormals() {
    const char* e = DevEnv("TCNYC_AO_FLAT");
    return e && *e == '1';
}

// A vertical flat surface keeps only this fraction of its screen-space AO. Walls, signs and
// billboards have no shape of their own to explain an AO band, so the band's edge reads as a hard
// dark line that slides with the camera; halving the darkening halves the edge's contrast without
// removing the shading. 0 hides it entirely, 1 is the old behaviour. TCNYC_AO_WALL overrides.
inline bool AoIgnoreFlatWalls() {
    const char* e = DevEnv("TCNYC_AO_FLATS");
    return !(e && *e == '0');
}

inline float AoWallKeep() {
    return std::clamp(AoEnvFloat("TCNYC_AO_WALL", 0.5f), 0.0f, 1.0f);
}

// Temporal AO: accumulate the AO factor across frames, reprojected with the camera, so the GTAO
// kernel can use fewer slices/steps. TCNYC_AO_TEMPORAL=1 turns it on (D3D12 experiment).
inline bool AoTemporal() {
    const char* e = DevEnv("TCNYC_AO_TEMPORAL");
    return e && *e == '1';
}

inline float AoTemporalBlend() {
    return std::clamp(AoEnvFloat("TCNYC_AO_TEMPORAL_BLEND", 0.8f), 0.0f, 0.99f);
}

inline int AoTemporalSlices() {
    const char* e = DevEnv("TCNYC_AO_TEMPORAL_SLICES");
    int n = e && *e ? std::atoi(e) : 2;
    return std::clamp(n, 1, 4);
}

inline int AoTemporalSteps(uint32_t mode) {
    (void)mode;
    const char* e = DevEnv("TCNYC_AO_TEMPORAL_STEPS");
    int n = e && *e ? std::atoi(e) : 4;  // 2 slices x 4 steps x 2 sides = 16 reads, vs 48 before
    return std::clamp(n, 1, 12);
}

// How strongly the resolve blur rejects taps across a depth step. Higher keeps creases crisp
// (and can leave a hard AO-band edge); lower softens the transition. TCNYC_AO_BLUR_DEPTH overrides.
inline float AoBlurDepth() {
    return std::max(0.01f, AoEnvFloat("TCNYC_AO_BLUR_DEPTH", 50.0f));
}

// Resolve blur width in AO texels (4 = the old 5x5 tent, 8 = a 9x9 tent). TCNYC_AO_BLUR_TAPS.
inline int AoBlurTaps() {
    const char* e = DevEnv("TCNYC_AO_BLUR_TAPS");
    int n = e && *e ? std::atoi(e) : 4;
    n = std::clamp(n, 2, 8);
    return n;
}

// TCNYC_AO_STRENGTH / TCNYC_AO_RADIUS override these for tuning without a rebuild
inline const float kAoStrength = AoEnvFloat("TCNYC_AO_STRENGTH", 1.0f);
inline const float kAoRadius = AoEnvFloat("TCNYC_AO_RADIUS", 0.75f);

// thin: a line or point draw. Those cover a pixel or two, so at half-resolution AO their footprint
// lands on a different depth texel as the camera moves: a wire in front of a facade then draws a
// dark line on the wall that flickers while moving. They still receive AO, but do not feed the depth
// prepass. (A one- or two-pixel triangle edge is the same case and is handled by the kernel's
// isolated-depth-sample test.)
inline uint8_t AoRoles(const State& s, bool samples_colour_rt, bool thin = false) {
    constexpr uint32_t kZero = 0, kOne = 1, kSrcAlpha = 0x302, kInvSrcAlpha = 0x303, kOpAdd = 0x8006;
    if (!s.rs[kRsZEnable]) return 0;
    if (samples_colour_rt) return 0;  // water, mirrors: sampling a colour target
    bool solid = true, alpha = false;
    if (SimpleState(s, kMBlendEnable)) {
        const uint32_t op = SimpleState(s, kMBlendOp);
        const uint32_t src = SimpleState(s, kMSrcBlend), dst = SimpleState(s, kMDestBlend);
        alpha = src == kSrcAlpha && dst == kInvSrcAlpha && (op == kOpAdd || op == 0);
        solid = src == kOne && dst == kZero && (op == kOpAdd || op == 0);
    }
    if (!solid && !alpha) return 0;  // an effect
    const bool zwrite = SimpleState(s, kMZWriteEnable) != 0;
    const bool colour = (SimpleState(s, kMColorWriteEnable) & 0x00FFFFFF) != 0;
    uint8_t roles = 0;
    if (zwrite && !thin) roles |= kAoDepth;
    // a colour pass over depth it (or its depth-only pass) wrote; alpha passes may skip writing
    if (colour && (zwrite || alpha)) roles |= kAoColour;
    return roles;
}

// The AO kernel and blur. TCNYC_VULKAN is 1 in the Vulkan shader compiler and 0 on D3D12.
// The binding/push-constant and full-screen NDC differences are the only splits.
inline const char* kAoHlsl = R"(
#if TCNYC_VULKAN
#if DEPTH_MS
[[vk::binding(0, 0)]] Texture2DMS<float> depthTex;
#else
[[vk::binding(0, 0)]] Texture2D<float> depthTex;
#endif
[[vk::binding(2, 0)]] SamplerState ps;
[[vk::push_constant]] cbuffer B { float4 src; float4 ao; float4 ao_up; };
#else
#if AO_TEMPORAL
// ao_t: this frame's noise offsets (x slice/rotation, y step), so the temporal history averages
// many kernel rotations instead of the same screen-locked pattern every frame
cbuffer B : register(b1) { float4 src; float4 ao; float4 ao_up; float4 ao_t; };
#else
cbuffer B : register(b1) { float4 src; float4 ao; float4 ao_up; };
#endif
#if DEPTH_MS
Texture2DMS<float> depthTex : register(t0);
#else
Texture2D<float> depthTex : register(t0);
#endif
SamplerState ps : register(s1);
#endif
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
#if DEPTH_MS
float RawDepth(float2 uv) {
    uint w, h, samples;
    depthTex.GetDimensions(w, h, samples);
    // the texel under uv: (i + 0.5) / size lands on i. Scaling by size - 1 drifted off the grid
    // and read one row and one column twice, drawing a line across the AO.
    uint2 p = min(uint2(saturate(uv) * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1));
    return depthTex.Load(p, 0);
}
#else
float RawDepth(float2 uv) {
    return depthTex.SampleLevel(ps, saturate(uv), 0);
}
#endif
// The AO kernel reads the prepass's distance target: each pixel holds the view distance (clip w),
// already linear, so no near/far is needed. Empty pixels hold kAoDistClear.
float LinearDepth(float raw) { return raw; }
// view-space position: the screen offset scales with depth (the projection's x / z, y / z)
float3 ViewAt(float2 uv, float z) {
    float2 n = uv * 2.0 - 1.0;
    return float3(n.x * ao.w * ao.z * z, -n.y * ao.z * z, z);
}
// Each 4x4 block of AO pixels covers 16 evenly spread kernel rotations (an ordered 4x4 pattern);
// the occlusion pass averages exactly one block, so the pattern cancels instead of reading as noise.
static const float kRot4x4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// GTAO (Jimenez et al. 2016, after Intel's XeGTAO): per slice through the pixel, find the highest
// occluder on each side (the horizons) and integrate the cosine-weighted visible arc between
// them. GTAO_SLICES and GTAO_STEPS come from the pass (3 x 6 at Half, 3 x 8 at Full).
#ifndef GTAO_SLICES
#define GTAO_SLICES 3
#define GTAO_STEPS 6
#endif
float FastAcos(float x) {
    const float a = abs(x);
    const float r = (-0.156583 * a + 1.570796) * sqrt(1.0 - a);
    return x >= 0 ? r : 3.141593 - r;
}
#if AO_FLATS
// How much of this pixel is a vertical flat surface: 1 for a wall, sign or billboard, fading out
// by about 37 degrees of tilt from vertical. A screen-space AO band on such a surface has no shape
// under it to explain the edge, so the edge reads as a hard dark line sliding with the camera.
// ao_up.w scales how much of that AO to hide (0 keeps all of it, 1 hides it entirely), so walls
// can keep part of their shading. Floors, roofs, roads and props always keep theirs.
float AoFlatWall(float3 n) {
    return saturate((0.6 - abs(dot(n, ao_up.xyz))) / 0.4) * ao_up.w;
}
#endif
// SSAO (app::Settings::ao_method 1, AO_SSAO): the earlier hemisphere kernel, restored as a choice beside GTAO.
// About 17 depth reads per pixel; cheaper than GTAO but grainier. Shares the GTAO pass's blur and resolve.
float AoFactor(float2 uv_in, uint2 pix) {
    // Work at the centre of the depth texel actually read. At half resolution an AO pixel's
    // centre falls between two depth texels; placing one texel's depth at that in-between spot
    // put the point off its own surface by half a texel of depth, worst on grazing floors.
    const float2 uv = (floor(uv_in / src.xy) + 0.5) * src.xy;
    float z = LinearDepth(RawDepth(uv));
    if (z >= src.w * 0.999) return 1.0;
    float3 p = ViewAt(uv, z);
    // Surface direction from the neighbour on the same surface: of each pair, the one with the
    // smaller depth step. Central differences bend the normal across creases and curb edges,
    // which sent samples into the surface and drew dark lines along every edge.
    const float2 ox = float2(src.x, 0), oy = float2(0, src.y);
    const float zr = LinearDepth(RawDepth(uv + ox)), zl = LinearDepth(RawDepth(uv - ox));
    const float zd = LinearDepth(RawDepth(uv + oy)), zu = LinearDepth(RawDepth(uv - oy));
    const float3 dx = abs(zr - z) < abs(z - zl) ? ViewAt(uv + ox, zr) - p : p - ViewAt(uv - ox, zl);
    const float3 dy = abs(zd - z) < abs(z - zu) ? ViewAt(uv + oy, zd) - p : p - ViewAt(uv - oy, zu);
    float3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;
#if AO_FLATS
    const float wall_hide = AoFlatWall(n);
    if (wall_hide >= 0.999) return 1.0;
#endif
    float3 t = normalize(cross(abs(n.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0), n));
    float3 b = cross(n, t);
#if AO_TEMPORAL
    const float ang = frac((kRot4x4[(pix.y & 3) * 4 + (pix.x & 3)] + 0.5) / 16.0 + ao_t.x) * 6.2831853;
#else
    const float ang = (kRot4x4[(pix.y & 3) * 4 + (pix.x & 3)] + 0.5) / 16.0 * 6.2831853;
#endif
    float cs = cos(ang), sn = sin(ang);
    // 12 hemisphere directions on a spiral: three rings of four, short and steep to long and flat
    static const float3 kernel[12] = {
        float3(0.25, 0.0, 0.35), float3(0.0, 0.25, 0.35), float3(-0.25, 0.0, 0.35), float3(0.0, -0.25, 0.35),
        float3(0.42, 0.42, 0.45), float3(-0.42, 0.42, 0.45), float3(-0.42, -0.42, 0.45), float3(0.42, -0.42, 0.45),
        float3(0.0, 0.9, 0.3), float3(-0.9, 0.0, 0.3), float3(0.0, -0.9, 0.3), float3(0.9, 0.0, 0.3)
    };
    // The depth units are the title's w-buffer scale, not metres, so the radius is a fraction of
    // the distance (ao.y scales it): about the same screen area near and far.
    const float rad = ao.y * 0.05 * z;
    float occlusion = 0.0;
    float count = 0.0;
    [unroll] for (int i = 0; i < 12; i++) {
        float3 d = kernel[i];
        d.xy = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
        float3 sp = p + (t * d.x + b * d.y + n * d.z) * rad;
        if (sp.z <= 0.001) continue;
        float2 suv = float2(sp.x / (sp.z * ao.w * ao.z), -sp.y / (sp.z * ao.z)) * 0.5 + 0.5;
        if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1) continue;
        // Test the pixel actually read, at its own 3D position: it occludes only if it rises above
        // this surface's plane within the radius. Comparing the sample point's depth with the
        // depth of the nearest pixel instead counted that pixel's grid offset on grazing floors
        // as occlusion, which beat against the pixel grid as regular stripes.
        const float2 cuv = (floor(suv / src.xy) + 0.5) * src.xy;
        const float3 v = ViewAt(cuv, LinearDepth(RawDepth(cuv))) - p;
        const float dist = length(v);
        count += 1.0;
        if (dist > 1e-6 && dist < rad) {
            const float h = dot(v, n) / dist;  // sine of the occluder's elevation above the plane
            if (h > 0.15) occlusion += (h - 0.15) / 0.85 * (1.0 - dist / rad);
        }
    }
    float f = saturate(1.0 - (occlusion / max(count, 1.0)) * ao.x);
#if AO_FLATS
    f = lerp(f, 1.0, wall_hide);
#endif
    return f;
}

float AoGtao(float2 uv_in, uint2 pix) {
    // Work at the centre of the depth texel actually read. At half resolution an AO pixel's
    // centre falls between two depth texels; placing one texel's depth at that in-between spot
    // put the point off its own surface by half a texel of depth, worst on grazing floors.
    const float2 uv = (floor(uv_in / src.xy) + 0.5) * src.xy;
    const float z = LinearDepth(RawDepth(uv));
#if AO_THIN
    // depth change per AO pixel, so the thin-sample test below does not reject a grazing floor
    const float2 z_slope = float2(ddx(z), ddy(z));
#endif
    if (z >= src.w * 0.999) return 1.0;
    const float3 p = ViewAt(uv, z);
    const float3 V = normalize(-p);
    // Normal from all four neighbour pairs, each weighted by how small its depth steps are (as
    // XeGTAO does). Picking one side per axis flipped between "floor" and "wall" along a crease's
    // raster stair steps, and the AO jumped with it: a beaded line on every crease. The weighted
    // blend gives a crease one steady in-between normal; a silhouette's big jump still drops out.
    const float2 ox = float2(src.x, 0), oy = float2(0, src.y);
    const float zr = LinearDepth(RawDepth(uv + ox)), zl = LinearDepth(RawDepth(uv - ox));
    const float zd = LinearDepth(RawDepth(uv + oy)), zu = LinearDepth(RawDepth(uv - oy));
    const float3 vr = ViewAt(uv + ox, zr) - p, vl = ViewAt(uv - ox, zl) - p;
    const float3 vd = ViewAt(uv + oy, zd) - p, vu = ViewAt(uv - oy, zu) - p;
    const float er = 1.0 / (1.0 + abs(zr - z) / (0.02 * z)), el = 1.0 / (1.0 + abs(zl - z) / (0.02 * z));
    const float ed = 1.0 / (1.0 + abs(zd - z) / (0.02 * z)), eu = 1.0 / (1.0 + abs(zu - z) / (0.02 * z));
    float3 q0 = normalize(cross(vr, vd)), q1 = normalize(cross(vd, vl));
    float3 q2 = normalize(cross(vl, vu)), q3 = normalize(cross(vu, vr));
    if (dot(q0, V) < 0.0) q0 = -q0;
    if (dot(q1, V) < 0.0) q1 = -q1;
    if (dot(q2, V) < 0.0) q2 = -q2;
    if (dot(q3, V) < 0.0) q3 = -q3;
    float3 n = q0 * (er * ed) + q1 * (ed * el) + q2 * (el * eu) + q3 * (eu * er);
    n = dot(n, n) > 1e-12 ? normalize(n) : V;
    if (dot(n, V) < 0.0) n = -n;
#if AO_FLAT
    // On a flat wall a coplanar triangle seam perturbs the four-pair normal just enough to leave
    // a dark line along the seam. The derivative normal is continuous across a flat surface, so
    // use it when at least three of the four neighbours are on this surface. A crease or a
    // silhouette has two or more large depth steps, where the four-pair normal is the stable one.
    {
        const float a = abs(zr - z), b = abs(zl - z), c = abs(zd - z), d = abs(zu - z);
        const float s0 = min(a, b), s1 = max(a, b), s2 = min(c, d), s3 = max(c, d);
        const float t1 = max(s0, s2), t2 = min(s1, s3);
        const float third = max(t1, t2);  // the third smallest of the four steps
        const float flat = saturate(1.0 - third / (0.02 * z));
        if (flat > 0.0) {
            float3 nd = cross(ddx(p), ddy(p));
            const float nd2 = dot(nd, nd);
            nd = nd2 > 1e-12 ? nd * rsqrt(nd2) : V;
            if (dot(nd, V) < 0.0) nd = -nd;
            n = normalize(lerp(n, nd, flat));
            if (dot(n, V) < 0.0) n = -n;
        }
    }
#endif
#if AO_FLATS
    const float wall_hide = AoFlatWall(n);
    if (wall_hide >= 0.999) return 1.0;
#endif

    // radius as a fraction of the distance (the depth units are the title's, not metres); in uv
    // it is then the same at every distance
    const float rad = ao.y * 0.05 * z;
    const float2 rad_uv = float2(rad / (z * ao.w * ao.z), rad / (z * ao.z)) * 0.5;
    const float falloff_from = rad * 0.385;  // full weight inside this, fading to 0 at rad

    // 4x4 ordered pattern: slice rotation from one index, step offset from another
    const uint idx = (pix.y & 3) * 4 + (pix.x & 3);
#if AO_TEMPORAL
    // A constant shift keeps each 4x4 block an even spread of rotations (the resolve still cancels
    // it within the frame), while successive frames fill in the rotations between them.
    const float slice_noise = frac((kRot4x4[idx] + 0.5) / 16.0 + ao_t.x);
    const float step_noise = frac(0.5 + kRot4x4[(idx + 5) & 15] * 0.618034 + ao_t.y);
#else
    const float slice_noise = (kRot4x4[idx] + 0.5) / 16.0;
    const float step_noise = frac(0.5 + kRot4x4[(idx + 5) & 15] * 0.618034);
#endif

    float visibility = 0.0;
    [unroll] for (int s = 0; s < GTAO_SLICES; s++) {
        const float phi = (float(s) + slice_noise) / GTAO_SLICES * 3.141593;
        const float2 omega = float2(cos(phi), sin(phi));
        const float3 dir = float3(omega, 0.0);  // view space: x right, y up
        const float2 step_uv = float2(omega.x, -omega.y) * rad_uv;  // uv: y down
        const float3 ortho = dir - dot(dir, V) * V;
        const float3 axis = normalize(cross(dir, V));
        const float3 proj_n = n - axis * dot(n, axis);
        const float proj_len = length(proj_n);
        const float sgn = dot(ortho, proj_n) >= 0.0 ? 1.0 : -1.0;
        const float cos_n = saturate(dot(proj_n, V) / max(proj_len, 1e-5));
        const float ang_n = sgn * FastAcos(cos_n);
        const float low0 = cos(ang_n + 1.570796), low1 = cos(ang_n - 1.570796);
        float hc0 = low0, hc1 = low1;
        [unroll] for (int j = 0; j < GTAO_STEPS; j++) {
            float t = (float(j) + step_noise) / GTAO_STEPS;
            t = t * t;  // denser near the pixel, where contact occlusion lives
            const float2 off = step_uv * t;
            [unroll] for (int side = 0; side < 2; side++) {
                const float2 suv = side == 0 ? uv + off : uv - off;
                if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1) continue;
                const float2 cuv = (floor(suv / src.xy) + 0.5) * src.xy;
                const float zc = LinearDepth(RawDepth(cuv));
                const float3 d = ViewAt(cuv, zc) - p;
                const float dist = length(d);
                // Outside the radius: skip, rather than pull this side's horizon back down. A
                // character standing in front of a wall hides the wall's crease behind it; resetting
                // on every step that hit the character erased the crease and bent the corner shadow
                // around the silhouette. Skipping keeps what the other steps found.
                if (dist < 1e-6 || dist >= rad) continue;
#if AO_THIN
                // A one- or two-pixel depth island (a wire or cable, a thin railing, or a mesh seam)
                // is not a real occluder: its screen footprint lands on a different depth texel as
                // the camera moves, so it draws a dark line on the wall that flickers while moving.
                // If both neighbours across the ray sit on the centre's surface while this sample
                // does not, skip it. The threshold follows the local depth slope so a grazing
                // floor's ordinary per-pixel step is not mistaken for an island.
                const float thick = max(0.01 * z, 0.75 * (abs(z_slope.x) + abs(z_slope.y)));
                if (abs(zc - z) > thick) {
                    const float2 texel_dir = float2(-step_uv.y / max(src.x, 1e-8), step_uv.x / max(src.y, 1e-8));
                    const float2 perp = normalize(texel_dir) * float2(src.x, src.y);
                    const float2 ua = cuv + perp, ub = cuv - perp;
                    if (ua.x >= 0 && ua.x <= 1 && ua.y >= 0 && ua.y <= 1 &&
                        ub.x >= 0 && ub.x <= 1 && ub.y >= 0 && ub.y <= 1) {
                        const float za = LinearDepth(RawDepth(ua));
                        const float zb = LinearDepth(RawDepth(ub));
                        if (abs(za - z) <= thick && abs(zb - z) <= thick) continue;
                    }
                }
#endif
                const float w = saturate((rad - dist) / (rad - falloff_from));
                const float low = side == 0 ? low0 : low1;
                const float hc = lerp(low, dot(d / dist, V), w);
                if (side == 0) hc0 = max(hc0, hc); else hc1 = max(hc1, hc);
            }
        }
        // horizon angles, side 1 negative; clamped to the hemisphere around the normal
        float h0 = -FastAcos(hc1), h1 = FastAcos(hc0);
        h0 = ang_n + max(h0 - ang_n, -1.570796);
        h1 = ang_n + min(h1 - ang_n, 1.570796);
        const float sin_n = sin(ang_n);
        const float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - ang_n)) * 0.25;
        const float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - ang_n)) * 0.25;
        visibility += proj_len * (arc0 + arc1);
    }
    visibility = saturate(visibility / GTAO_SLICES);
    float f = pow(visibility, ao.x);  // strength as a power, as XeGTAO does
#if AO_FLATS
    f = lerp(f, 1.0, wall_hide);
#endif
    return f;
}
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
float4 psmain(O i) : SV_Target {
#ifdef AO_SSAO
    float f = AoFactor(i.uv, uint2(i.p.xy));
#else
    float f = AoGtao(i.uv, uint2(i.p.xy));
#endif
    return float4(f, f, f, 1.0);
}
)";

inline const char* kAoOcclusionHlsl = R"(
#if TCNYC_VULKAN
[[vk::binding(0, 0)]] Texture2D<float> aoTex;
#if DEPTH_MS
[[vk::binding(1, 0)]] Texture2DMS<float> depthTex;
#else
[[vk::binding(1, 0)]] Texture2D<float> depthTex;
#endif
[[vk::binding(2, 0)]] SamplerState ls;
[[vk::binding(3, 0)]] SamplerState ps;
[[vk::push_constant]] cbuffer B { float4 src; float4 planes; };
#else
cbuffer B : register(b1) { float4 src; float4 planes; };
Texture2D<float> aoTex : register(t0);
#if DEPTH_MS
Texture2DMS<float> depthTex : register(t1);
#else
Texture2D<float> depthTex : register(t1);
#endif
SamplerState ls : register(s0);
SamplerState ps : register(s1);
#endif
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
#if DEPTH_MS
float RawDepth(float2 uv) {
    uint w, h, samples;
    depthTex.GetDimensions(w, h, samples);
    // the texel under uv: (i + 0.5) / size lands on i. Scaling by size - 1 drifted off the grid
    // and read one row and one column twice, drawing a line across the AO.
    uint2 p = min(uint2(saturate(uv) * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1));
    return depthTex.Load(p, 0);
}
#else
float RawDepth(float2 uv) { return depthTex.SampleLevel(ps, saturate(uv), 0); }
#endif
float LinearDepth(float raw) { return raw; }  // the prepass's view distance
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
float4 psmain(O i) : SV_Target {
    const float2 t = src.xy;
    const float z0 = LinearDepth(RawDepth(i.uv));
    // depth per screen pixel, taken before any branch so the derivatives are defined
    const float2 slope = float2(ddx(z0), ddy(z0));
    const float2 uv_per_px = float2(ddx(i.uv).x, ddy(i.uv).y);
    // No geometry in the AO depth here. Either the sky, or a pixel gap at a mesh seam (curb top
    // meeting road) that a later pass covers in the real image: forcing 0 there drew a row of
    // bright dots along every such seam. Fill it from the geometry around it instead; real sky
    // has none and stays at 0.
    const float sky = planes.y * 0.999;
    if (z0 >= sky) {
        const float2 c0 = floor(i.uv / t - 0.5) - 1.0;
        float fa = 0.0, fn = 0.0;
        [unroll] for (int fy = 0; fy < 4; fy++) {
            [unroll] for (int fx = 0; fx < 4; fx++) {
                const float2 fuv = (c0 + float2(float(fx), float(fy)) + 0.5) * t;
                if (LinearDepth(RawDepth(fuv)) >= sky) continue;
                fa += aoTex.SampleLevel(ps, fuv, 0).r;
                fn += 1.0;
            }
        }
        const float focc = fn > 0.0 ? 1.0 - saturate(fa / fn) : 0.0;
        return float4(focc, focc, focc, 1);
    }
    // A 4x4 block of AO texel centres: any 4x4 window holds each of the 16 kernel rotations once,
    // so the rotation pattern averages out. Each tap is weighted by how far its depth is from the
    // centre's surface plane (the centre depth extended along its screen slope), not from the
    // centre depth itself: on a floor seen at a grazing angle every row is deeper than the last,
    // and comparing raw depths dropped the rows above and below, leaving horizontal stripes (worst
    // at half resolution, where an AO row spans two screen rows). Real edges still fall off.
    // The 4x4 texel window is centred on this screen pixel's position in AO texels, not snapped
    // to the AO texel it falls in: at half resolution the snapped window gave every 2x2 group of
    // screen pixels the same value (blocky) and picked the wrong side of edges (dotted curbs).
    // Each tap sits on the corner between four AO texels and reads them through the linear
    // sampler, so the same 16 fetches average a 5x5 texel area with a tent falloff instead of
    // a hard 4x4 block: smoother, at no extra cost.
    // AO_BLUR_TAPS x AO_BLUR_TAPS corners around the pixel: 4 is the old 5x5 tent, 8 is 9x9.
    const float2 first = floor(i.uv / t - 0.5) - float(AO_BLUR_TAPS / 2 - 1);
    float a = 0.0, wsum = 0.0;
    [unroll] for (int y = 0; y < AO_BLUR_TAPS; y++) {
        [unroll] for (int x = 0; x < AO_BLUR_TAPS; x++) {
            const float2 uv = (first + float2(float(x), float(y)) + 1.0) * t;  // a texel corner
            const float zk = LinearDepth(RawDepth(uv));
            if (zk >= sky) continue;  // a seam gap or the sky: its "no occlusion" would lighten this pixel
            // The smaller of two depth errors: from the centre's surface plane (keeps grazing floors
            // averaging, see above) or from the centre depth itself. On a crease the centre straddles
            // two surfaces, its slope is meaningless and the plane test rejected the neighbouring
            // crease pixels, leaving the rotation pattern as a beaded line along every crease.
            const float zp = z0 + dot((uv - i.uv) / uv_per_px, slope);
            const float err = min(abs(zk - zp), abs(zk - z0));
            const float wgt = 1.0 / (1.0 + err / max(z0, 1e-4) * AO_BLUR_DEPTH);
            a += aoTex.SampleLevel(ls, uv, 0).r * wgt;
            wsum += wgt;
        }
    }
    const float occ = wsum > 0.0 ? 1.0 - saturate(a / wsum) : 0.0;
    return float4(occ, occ, occ, 1.0);
}
)";

// temporal: the AO factor is accumulated across frames, so use fewer slices/steps.
inline std::string AoHorizonSource(uint32_t mode, bool depth_ms, bool ssao = false, bool temporal = false) {
    const int slices = temporal ? AoTemporalSlices() : 3;
    const int steps = temporal ? AoTemporalSteps(mode) : (mode == 1 ? 6 : 8);
    return std::string(ssao ? "#define AO_SSAO 1\n" : "") + (temporal ? "#define AO_TEMPORAL 1\n" : "") +
           "#define DEPTH_MS " + (depth_ms ? "1\n" : "0\n") +
           "#define AO_THIN " + (AoThinSamples() ? "1\n" : "0\n") +
           "#define AO_FLAT " + (AoFlatNormals() ? "1\n" : "0\n") +
           "#define AO_FLATS " + (AoIgnoreFlatWalls() ? "1\n" : "0\n") + "#define GTAO_SLICES " +
           std::to_string(slices) + "\n#define GTAO_STEPS " + std::to_string(steps) + "\n" + kAoHlsl;
}

// Temporal AO resolve (D3D12): reproject the previous AO, reject it where the surface is not the
// same, clip it to the current neighbourhood and blend. Every world pixel reprojects: with the
// draw's own motion where the AO prepass wrote one, otherwise with the camera from the prepass's
// hardware depth (the same reconstruction TAA's resolveMotion uses). Writes the next history:
// .r the AO factor, .g the linear view depth (camera units) that produced it.
// t0 current AO (AO resolution), t1 AO prepass depth (full size, reversed), t2 previous history,
// t3 AO prepass motion (full size; 60000 where a draw had no previous position).
inline const char* kAoTemporalHlsl = R"(
Texture2D<float> aoTex : register(t0);
Texture2D<float> depthTex : register(t1);
Texture2D<float2> histTex : register(t2);
Texture2D<float4> motionTex : register(t3);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
cbuffer B : register(b1) {
    float4 m0; float4 m1; float4 m2; float4 m3;  // current view -> previous view (row vectors)
    float4 proj;   // current xs ys, previous xs ys
    float4 z;      // near, far (view units), unused, unused
    float4 px;     // this frame's jitter in uv (x, y), unused
    float4 flags;  // x history valid, y blend, z object motion available, w unused
};
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float kNoHistory = 65000.0;  // .g of a sky / empty texel: never matches a surface
float ViewDepth(float d) { return z.x * z.y / max(z.y - (1.0 - d) * (z.y - z.x), 1e-4); }
float4 psmain(O i) : SV_Target {
    uint w, h;
    aoTex.GetDimensions(w, h);
    const int2 p = int2(i.p.xy);
    const float current = aoTex.Load(int3(p, 0));
    const float d = depthTex.SampleLevel(ps, i.uv, 0);
    if (d <= 1e-7) return float4(current, kNoHistory, 0, 1);  // the clear: sky or effects only
    const float vz = ViewDepth(d);

    // Where this surface was last frame, and the view depth it had there.
    float2 motion = 0;
    float expect = vz;
    float tol = 0.04;
    bool have = false;
    if (flags.z > 0.5) {
        const float2 object = motionTex.SampleLevel(ps, i.uv, 0).xy;
        if (all(abs(object) < 50000.0)) {
            motion = object;
            have = true;
            tol = 0.1;  // a moving object's depth along the view changes by an unknown amount
        }
    }
    if (!have) {
        const float2 uv = i.uv - px.xy;
        const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        const float4 view = float4(ndc.x * vz / proj.x, ndc.y * vz / proj.y, vz, 1.0);
        const float4 prev = view.x * m0 + view.y * m1 + view.z * m2 + view.w * m3;
        if (prev.z > 1e-3) {
            const float2 pndc = prev.xy * proj.zw / prev.z;
            motion = float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) - uv;
            expect = prev.z;
            have = all(isfinite(motion));
        }
    }

    // Bilinear history, but each of the four texels counts only if it held this surface: a plain
    // bilinear fetch blends the background into a silhouette's edge, which is what trailed
    // behind edges while the camera turned.
    float history = 0.0, wsum = 0.0;
    const float2 suv = i.uv + motion;
    if (flags.x > 0.5 && have && all(suv > 0.0) && all(suv < 1.0)) {
        const float2 pos = suv * float2(w, h) - 0.5;
        const int2 base = int2(floor(pos));
        const float2 f = pos - float2(base);
        [unroll] for (int k = 0; k < 4; k++) {
            const int2 o = int2(k & 1, k >> 1);
            const int2 q = clamp(base + o, int2(0, 0), int2(int(w) - 1, int(h) - 1));
            const float2 s = histTex.Load(int3(q, 0));
            const float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
            const float dw = abs(s.y - expect) <= tol * expect + 0.01 ? 1.0 : 0.0;
            history += s.x * bw * dw;
            wsum += bw * dw;
        }
    }
    if (wsum < 0.05) return float4(current, vz, 0, 1);  // disoccluded or new: this frame only
    history /= wsum;

    // Clip the history to the current 3x3 neighbourhood (mean +- 1.5 sigma, within min/max) so an
    // AO change the reprojection cannot explain (a moving occluder, view-dependent horizons) is
    // followed within a frame or two instead of lagging.
    float m1s = 0.0, m2s = 0.0, lo = current, hi = current;
    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            const int2 q = clamp(p + int2(x, y), int2(0, 0), int2(int(w) - 1, int(h) - 1));
            const float s = aoTex.Load(int3(q, 0));
            m1s += s;
            m2s += s * s;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    const float mean = m1s / 9.0;
    const float sigma = sqrt(max(m2s / 9.0 - mean * mean, 0.0));
    history = clamp(history, max(lo, mean - 1.5 * sigma), min(hi, mean + 1.5 * sigma));

    // Fast motion resamples the history every frame, which softens it: trust it a little less,
    // and less again where only part of the 2x2 footprint matched (a silhouette).
    const float speed = length(motion * float2(w, h));  // AO texels this frame
    const float weight = flags.y * lerp(1.0, 0.6, saturate(speed / 24.0)) * saturate(wsum * 1.5);
    return float4(lerp(current, history, weight), vz, 0, 1);
}
)";

inline std::string AoResolveSource(bool depth_ms) {
    return std::string("#define DEPTH_MS ") + (depth_ms ? "1\n" : "0\n") + "#define AO_BLUR_DEPTH " +
           std::to_string(AoBlurDepth()) + "\n#define AO_BLUR_TAPS " + std::to_string(AoBlurTaps()) + "\n" +
           kAoOcclusionHlsl;
}

}  // namespace gpu
