// Final presentation: draws the processed frame into the drawable, letter-
// or pillar-boxed to the chosen aspect ratio. Linear sampling here is the
// "bilinear" filter; the other filters run before this pass (PostProcess.metal).
//
// This is the app's own shader. Guest shaders (NV2A vertex programs and
// register-combiner setups) are translated to MSL at runtime by the GPU
// backend and never live in this file.
#include <metal_stdlib>
using namespace metal;

// Must match `PresentUniforms` in Frontend/Render/FrameRenderer.swift (16 bytes).
struct PresentUniforms {
    float4 rect; // x0, y0 (bottom-left), x1, y1 (top-right) in NDC
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut present_vertex(uint vertexID [[vertex_id]],
                                constant PresentUniforms &u [[buffer(0)]]) {
    // Triangle strip generated from the vertex id: (0,0) (1,0) (0,1) (1,1)
    float2 corner = float2(float(vertexID & 1), float(vertexID >> 1));
    VertexOut out;
    out.position = float4(mix(u.rect.xy, u.rect.zw, corner), 0.0, 1.0);
    out.uv = float2(corner.x, 1.0 - corner.y); // texture origin is top-left
    return out;
}

fragment float4 present_fragment(VertexOut in [[stage_in]],
                                 texture2d<float> image [[texture(0)]],
                                 sampler imageSampler [[sampler(0)]]) {
    return float4(image.sample(imageSampler, in.uv).rgb, 1.0);
}
