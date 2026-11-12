#pragma once
// The TAA passes both renderers share: resolveMotion (dense motion and metadata from depth, the camera and the
// draws' own motion), taa (the history blend), and the FSR/DLSS input and HUD passes D3D12 uses. TCNYC_VULKAN is 1
// in the Vulkan shader compiler and 0 on D3D12: the constants (push constants there), the full-screen triangle's
// NDC direction and the depth convention (Vulkan's is not reversed) are the splits. On Vulkan t0-t3 are bindings 2-5
// and s0/s1 bindings 6/7 (InjectBindings); MOTION_MS reads a multisampled motion target beside resolved depth.

namespace gpu {

inline const char* kTaaHlsl = R"(
#if TCNYC_VULKAN
[[vk::push_constant]] cbuffer B { float4 m0; float4 m1; float4 m2; float4 m3; float4 proj; float4 z; float4 px; float4 flags; };
#else
cbuffer B : register(b1) { float4 m0; float4 m1; float4 m2; float4 m3; float4 proj; float4 z; float4 px; float4 flags; };
#endif
Texture2D cur : register(t0);
Texture2D hist : register(t1);
#if TAA_RESOLVE
Texture2D<float2> denseMotion : register(t2);
Texture2D<float2> motionMetadata : register(t3);
#elif DEPTH_MS
Texture2DMS<float> depthTex : register(t2);
#define DEPTH_LOAD(p) depthTex.Load(p, 0)
Texture2DMS<float4> motionTex : register(t3);
#define MOTION_LOAD(p) motionTex.Load(p, 0)
#elif MOTION_MS
Texture2D<float> depthTex : register(t2);
#define DEPTH_LOAD(p) depthTex.Load(int3(p, 0))
Texture2DMS<float4> motionTex : register(t3);
#define MOTION_LOAD(p) motionTex.Load(p, 0)
#else
Texture2D<float> depthTex : register(t2);
#define DEPTH_LOAD(p) depthTex.Load(int3(p, 0))
Texture2D<float4> motionTex : register(t3);
#define MOTION_LOAD(p) motionTex.Load(int3(p, 0))
#endif
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
#if !TAA_RESOLVE
Texture2D<float2> fsrMetadata : register(t1);
struct FsrInputs {
    float depth : SV_Target0;
    float reactive : SV_Target1;
};
FsrInputs fsrInputs(O i) {
    const int2 pixel = int2(i.p.xy);
    FsrInputs o;
    const float2 metadata = fsrMetadata.Load(int3(pixel, 0));
    o.depth = metadata.x;
    o.reactive = 0;
    [branch] if (flags.x > 0 && metadata.y != -2) o.reactive = MOTION_LOAD(pixel).z;
    return o;
}
struct MotionOut {
    float2 velocity : SV_Target0;
    float2 metadata : SV_Target1;
};
MotionOut resolveMotion(O i) {
    const int2 pixel = int2(i.p.xy);
#if TCNYC_VULKAN
    const float depth = 1 - DEPTH_LOAD(pixel);  // Vulkan keeps the guest's 0 (near) to 1 depth; D3D12's is reversed
#else
    const float depth = DEPTH_LOAD(pixel);
#endif
    MotionOut o;
    o.velocity = 0;
    o.metadata = float2(depth, -1);
    float2 object = 60000;
    [branch] if (flags.x > 0) object = MOTION_LOAD(pixel).xy;
    if (object.x > 62000) {
        o.metadata.y = -2;
        return o;
    }
    if (z.w > 0) return o;

    // Reconstruct the world point at this input texel, undoing the projection jitter.
    const float2 uv = i.uv - px.xy;
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    const float w = z.x * z.y / max(z.y - (1 - depth) * (z.y - z.x), 1e-4) * z.z;
    const float4 view = float4(ndc.x * w / proj.x, ndc.y * w / proj.y, w, 1);
    const float4 prev = view.x * m0 + view.y * m1 + view.z * m2 + view.w * m3;
    const float2 pndc = prev.xy * proj.zw / max(prev.z, 1e-3);
    const float2 camera = float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) - uv;
    const bool cameraValid = prev.z > 1e-3 && all(isfinite(camera));

    if (all(abs(object) < 50000)) {
        o.velocity = clamp(object, -2, 2);
        o.metadata.y = 1 + (cameraValid ? min(length((object - camera) / px.zw), 4) : 4);
    } else if (cameraValid) {
        o.velocity = clamp(camera, -2, 2);
        o.metadata.y = 0;
    }
    return o;
}
#else
float4 fsrHud(O i) : SV_Target {
    uint width, height;
    motionMetadata.GetDimensions(width, height);
    const uint2 size = uint2(width, height);
    const int3 pixel = int3(min(uint2(i.uv * size), size - 1), 0);
    if (motionMetadata.Load(pixel).y != -2) discard;
    return cur.SampleLevel(ls, i.uv, 0);
}
float3 ToYCoCg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// the history through a 5-tap Catmull-Rom (bilinear taps): bilinear alone blurs a little more every frame in motion
float3 HistoryAt(float2 uv) {
    float2 size = 1.0 / px.zw;
    float2 pos = uv * size;
    float2 c1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - c1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (c1 - 1) * px.zw, t3 = (c1 + 2) * px.zw, t12 = (c1 + w2 / w12) * px.zw;
    float3 r = hist.SampleLevel(ls, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y) +
               hist.SampleLevel(ls, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y) +
               hist.SampleLevel(ls, t12, 0).rgb * (w12.x * w12.y) +
               hist.SampleLevel(ls, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y) +
               hist.SampleLevel(ls, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    float n = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / n, 0.0);
}
float4 taa(O i) : SV_Target {
    int2 ip = int2(i.p.xy);
    // this frame shifted by its jitter: read it back where the pixel centre landed, so the blend does not wobble
    float3 c = cur.SampleLevel(ls, i.uv + px.xy, 0).rgb;
    if (z.w > 0) return float4(c, 1);
    // the neighbourhood's mean and spread in YCoCg, and the nearest depth around (edges move with the nearer surface)
    float3 sum1 = 0, sum2 = 0;
    float near_depth = -1;
    float motion_kind = -1;
    int2 near_at = ip;
    const int2 last = int2(round(1.0 / px.zw)) - 1;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            float3 n = ToYCoCg(cur.Load(int3(ip + int2(x, y), 0)).rgb);
            sum1 += n;
            sum2 += n * n;
            int2 at = clamp(ip + int2(x, y), 0, last);
            float2 info = motionMetadata.Load(int3(at, 0));
            if (info.x > near_depth) {
                near_depth = info.x;
                motion_kind = info.y;
                near_at = at;
            }
        }
    // TCNYC_TAA_VIEW=3: red where no motion is known, green the HUD (kept as drawn), blue the nearest depth
    if (flags.y > 2.5) return float4(motion_kind == -1 ? 1 : 0, motion_kind == -2 ? 1 : 0, saturate(near_depth), 1);
    if (motion_kind < 0) return float4(c, 1);
    float3 mean = sum1 / 9;
    float3 sigma = sqrt(max(sum2 / 9 - mean * mean, 0.0));
    const float2 velocity = denseMotion.Load(int3(near_at, 0));
    const float2 puv = i.uv + velocity;
    if (any(puv < 0) || any(puv > 1)) return float4(c, 1);
    // how far the pixel moved, in pixels: the clip is loose when still (stable) and tight in motion (no trails)
    float moved = length(velocity / px.zw);
    float motion = saturate(moved / 2.0);
    // an object moving by itself (its motion differs from the camera's by half a pixel or more): the strictest
    // handling; the world, whose motion is the camera's, keeps the steadier one
    const bool draw_motion = motion_kind >= 1;
    const float difference = draw_motion ? motion_kind - 1 : 0;
    const bool own = difference > 0.5;
    // variance clip: the history pulled toward the mean until it lies within the neighbourhood's spread
    float3 h = ToYCoCg(HistoryAt(puv));
    // loose where nothing moves (steady fine detail), tight where anything does (no trails); per-object motion makes
    // a car moving past a still view count as moving
    float3 e = sigma * (own ? 0.9 : lerp(1.6, 1.0, motion)) + 1e-4;
    float3 d = h - mean;
    float3 u = abs(d / e);
    float s = max(u.x, max(u.y, u.z));
    if (s > 1) h = mean + d / s;
    // more of the new frame the further the pixel moved, so fast motion does not trail; weighted by 1 / (1 + luma)
    // so a single bright pixel (a window, a lamp) cannot flicker the result
    float alpha = own ? max(0.25, lerp(0.08, 0.4, saturate(moved / 6.0))) : lerp(0.07, 0.3, saturate(moved / 8.0));
    float3 hc = FromYCoCg(h);
    // TCNYC_TAA_VIEW=1: the motion used, red and green its size across and down (16 pixels full), blue 1 where the
    // draw's own motion was used, 0 the camera's; =2: red how far the draw's motion is from the camera's (4 pixels
    // full), blue as in 1
    if (flags.y > 1.5) return float4(saturate(difference / 4), 0, draw_motion ? 1 : 0, 1);
    if (flags.y > 0.5) return float4(saturate(abs(velocity / px.zw) / 16), draw_motion ? 1 : 0, 1);
    float wc = alpha / (1 + ToYCoCg(c).x), wh = (1 - alpha) / (1 + h.x);
    return float4((c * wc + hc * wh) / (wc + wh), 1);
}
#endif
)";

}  // namespace gpu
