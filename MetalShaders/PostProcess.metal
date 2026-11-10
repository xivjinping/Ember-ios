// Post-processing compute kernels.
//
//   fxaa_kernel            edge-directed anti-aliasing at the internal resolution
//   upscale_sharpen_kernel resample to the output size + contrast-adaptive sharpening
//
// Both are single-pass and read each source texel a handful of times, so they
// cost well under a millisecond at 1080p on any A12-or-later GPU. The
// MetalFX spatial scaler (the "FSR-class" option) is Apple's own compute
// pass and is driven from Swift, not from this file.
#include <metal_stdlib>
using namespace metal;

static inline float luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

// Fast approximate anti-aliasing: find the local luma gradient, then blur
// along the edge (perpendicular to the gradient) — never across it, so
// textures and text stay sharp while stair-steps are smoothed.
kernel void fxaa_kernel(texture2d<float, access::sample> source [[texture(0)]],
                        texture2d<float, access::write> destination [[texture(1)]],
                        uint2 gid [[thread_position_in_grid]]) {
    const uint width = destination.get_width(), height = destination.get_height();
    if (gid.x >= width || gid.y >= height) return;

    constexpr sampler s(filter::linear, address::clamp_to_edge);
    const float2 texel = 1.0 / float2(width, height);
    const float2 uv = (float2(gid) + 0.5) * texel;

    const float3 centre = source.sample(s, uv, level(0)).rgb;
    const float lNW = luma(source.sample(s, uv + float2(-0.5, -0.5) * texel, level(0)).rgb);
    const float lNE = luma(source.sample(s, uv + float2( 0.5, -0.5) * texel, level(0)).rgb);
    const float lSW = luma(source.sample(s, uv + float2(-0.5,  0.5) * texel, level(0)).rgb);
    const float lSE = luma(source.sample(s, uv + float2( 0.5,  0.5) * texel, level(0)).rgb);
    const float lM  = luma(centre);

    const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // Skip flat areas: nothing to fix, and it keeps the pass cheap.
    if (lMax - lMin < max(0.0312, lMax * 0.125)) {
        destination.write(float4(centre, 1.0), gid);
        return;
    }

    float2 direction = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    const float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
    const float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, float2(-8.0), float2(8.0)) * texel;

    const float3 narrow = 0.5 * (source.sample(s, uv + direction * (1.0 / 3.0 - 0.5), level(0)).rgb +
                                 source.sample(s, uv + direction * (2.0 / 3.0 - 0.5), level(0)).rgb);
    const float3 wide = narrow * 0.5 + 0.25 * (source.sample(s, uv - direction * 0.5, level(0)).rgb +
                                               source.sample(s, uv + direction * 0.5, level(0)).rgb);
    // The wide blur is better when it stays within the local range; otherwise
    // it has reached across the edge and the narrow one is used.
    const float lWide = luma(wide);
    destination.write(float4((lWide < lMin || lWide > lMax) ? narrow : wide, 1.0), gid);
}

// Must match `SharpenUniforms` in Frontend/Render/FrameRenderer.swift.
struct SharpenUniforms {
    float sharpness; // 0 = mild, 1 = strong
};

// Bilinear resample to the destination size followed by contrast-adaptive
// sharpening: the amount is reduced where the neighbourhood already spans
// most of the range, which avoids halos on hard edges and HUD text.
kernel void upscale_sharpen_kernel(texture2d<float, access::sample> source [[texture(0)]],
                                   texture2d<float, access::write> destination [[texture(1)]],
                                   constant SharpenUniforms &u [[buffer(0)]],
                                   uint2 gid [[thread_position_in_grid]]) {
    const uint width = destination.get_width(), height = destination.get_height();
    if (gid.x >= width || gid.y >= height) return;

    constexpr sampler s(filter::linear, address::clamp_to_edge);
    const float2 uv = (float2(gid) + 0.5) / float2(width, height);
    const float2 step = 1.0 / float2(source.get_width(), source.get_height());

    const float3 c = source.sample(s, uv, level(0)).rgb;
    const float3 n = source.sample(s, uv + float2(0.0, -step.y), level(0)).rgb;
    const float3 b = source.sample(s, uv + float2(0.0,  step.y), level(0)).rgb;
    const float3 w = source.sample(s, uv + float2(-step.x, 0.0), level(0)).rgb;
    const float3 e = source.sample(s, uv + float2( step.x, 0.0), level(0)).rgb;

    const float3 lo = min(c, min(min(n, b), min(w, e)));
    const float3 hi = max(c, max(max(n, b), max(w, e)));
    const float3 headroom = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, float3(1e-4))));
    const float3 weight = headroom * (-1.0 / mix(8.0, 5.0, saturate(u.sharpness)));

    const float3 result = (c + (n + b + w + e) * weight) / (1.0 + 4.0 * weight);
    destination.write(float4(saturate(result), 1.0), gid);
}
