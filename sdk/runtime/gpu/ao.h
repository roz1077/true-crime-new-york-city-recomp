#pragma once

#include <cstdint>
#include <cstdlib>

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
    const char* e = std::getenv(name);
    return e && *e ? std::strtof(e, nullptr) : fallback;
}

// TCNYC_AO_STRENGTH / TCNYC_AO_RADIUS override these for tuning without a rebuild
inline const float kAoStrength = AoEnvFloat("TCNYC_AO_STRENGTH", 1.0f);
inline const float kAoRadius = AoEnvFloat("TCNYC_AO_RADIUS", 0.75f);

inline uint8_t AoRoles(const State& s, bool samples_colour_rt) {
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
    if (zwrite) roles |= kAoDepth;
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
[[vk::push_constant]] cbuffer B { float4 src; float4 ao; };
#else
cbuffer B : register(b1) { float4 src; float4 ao; };
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
// The title tests GREATEREQUAL and the host mirrors it to LESSEQUAL: host depth is 0 near, 1 far,
// and the AO depth prepass is cleared to 1.
// raw is the prepass's view distance (clip w): already linear, no near/far needed
float LinearDepth(float raw) { return raw; }
// view-space position: the screen offset scales with depth (the projection's x / z, y / z)
float3 ViewAt(float2 uv, float z) {
    float2 n = uv * 2.0 - 1.0;
    return float3(n.x * ao.w * ao.z * z, -n.y * ao.z * z, z);
}
float SafeDepth(float2 uv, float z) {
    const float d = LinearDepth(RawDepth(uv));
    return abs(d - z) < 0.05 * z ? d : z;
}
// Each 4x4 block of AO pixels covers 16 evenly spread kernel rotations (an ordered 4x4 pattern);
// the occlusion pass averages exactly one block, so the pattern cancels instead of reading as noise.
static const float kRot4x4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
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
    float3 t = normalize(cross(abs(n.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0), n));
    float3 b = cross(n, t);
    const float ang = (kRot4x4[(pix.y & 3) * 4 + (pix.x & 3)] + 0.5) / 16.0 * 6.2831853;
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
    return saturate(1.0 - (occlusion / max(count, 1.0)) * ao.x);
}

// GTAO (Jimenez et al. 2016, after Intel's XeGTAO): per slice through the pixel, find the highest
// occluder on each side (the horizons) and integrate the cosine-weighted visible arc between
// them. GTAO_SLICES and GTAO_STEPS come from the pass (2 x 6 at Half, 3 x 8 at Full).
#ifndef GTAO_SLICES
#define GTAO_SLICES 2
#define GTAO_STEPS 6
#endif
float FastAcos(float x) {
    const float a = abs(x);
    const float r = (-0.156583 * a + 1.570796) * sqrt(1.0 - a);
    return x >= 0 ? r : 3.141593 - r;
}
float AoGtao(float2 uv_in, uint2 pix) {
    // the depth texel actually read, as in AoFactor
    const float2 uv = (floor(uv_in / src.xy) + 0.5) * src.xy;
    const float z = LinearDepth(RawDepth(uv));
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

    // radius as a fraction of the distance (the depth units are the title's, not metres); in uv
    // it is then the same at every distance
    const float rad = ao.y * 0.05 * z;
    const float2 rad_uv = float2(rad / (z * ao.w * ao.z), rad / (z * ao.z)) * 0.5;
    const float falloff_from = rad * 0.385;  // full weight inside this, fading to 0 at rad

    // 4x4 ordered pattern: slice rotation from one index, step offset from another
    const uint idx = (pix.y & 3) * 4 + (pix.x & 3);
    const float slice_noise = (kRot4x4[idx] + 0.5) / 16.0;
    const float step_noise = frac(0.5 + kRot4x4[(idx + 5) & 15] * 0.618034);

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
                const float3 d = ViewAt(cuv, LinearDepth(RawDepth(cuv))) - p;
                const float dist = length(d);
                // Outside the radius: skip, rather than pull this side's horizon back down. A
                // character standing in front of a wall hides the wall's crease behind it; resetting
                // on every step that hit the character erased the crease and bent the corner shadow
                // around the silhouette. Skipping keeps what the other steps found.
                if (dist < 1e-6 || dist >= rad) continue;
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
    return pow(visibility, ao.x);  // strength as a power, as XeGTAO does
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
    float f = AoFactor(i.uv, uint2(i.p.xy));  // TCNYC_AO_METHOD=ssao: the earlier kernel
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
    const float2 first = floor(i.uv / t - 0.5) - 1.0;  // texel index of the window's first column/row
    float a = 0.0, wsum = 0.0;
    [unroll] for (int y = 0; y < 4; y++) {
        [unroll] for (int x = 0; x < 4; x++) {
            const float2 uv = (first + float2(float(x), float(y)) + 1.0) * t;  // a texel corner
            const float zk = LinearDepth(RawDepth(uv));
            if (zk >= sky) continue;  // a seam gap or the sky: its "no occlusion" would lighten this pixel
            // The smaller of two depth errors: from the centre's surface plane (keeps grazing floors
            // averaging, see above) or from the centre depth itself. On a crease the centre straddles
            // two surfaces, its slope is meaningless and the plane test rejected the neighbouring
            // crease pixels, leaving the rotation pattern as a beaded line along every crease.
            const float zp = z0 + dot((uv - i.uv) / uv_per_px, slope);
            const float err = min(abs(zk - zp), abs(zk - z0));
            const float wgt = 1.0 / (1.0 + err / max(z0, 1e-4) * 50.0);
            a += aoTex.SampleLevel(ls, uv, 0).r * wgt;
            wsum += wgt;
        }
    }
    const float occ = wsum > 0.0 ? 1.0 - saturate(a / wsum) : 0.0;
    return float4(occ, occ, occ, 1.0);
}
)";

}  // namespace gpu
