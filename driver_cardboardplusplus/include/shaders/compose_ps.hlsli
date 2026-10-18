// Side-by-side pixel shader: samples the left or right eye by screen half and
// converts to sRGB. Descriptor layout is binding 0 = cbuffer, 1 = sampler,
// 2 = t0 image, 3 = t1 image.
static const char* kVkSBSPSSource = R"(
Texture2D<float4> leftEye : register(t0);
Texture2D<float4> rightEye : register(t1);
SamplerState samp : register(s0);
cbuffer Bounds : register(b0) {
    float4 leftRect;
    float4 rightRect;
};
struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
float4 linearToSrgb(float4 c) {
    float3 lo = 12.92 * c.rgb;
    float3 hi = 1.055 * pow(c.rgb, 1.0/2.4) - 0.055;
    c.rgb = (c.rgb <= 0.0031308) ? lo : hi;
    return saturate(c);
}
float4 main(PSIn input) : SV_Target {
    float4 c;
    if (input.uv.x < 0.5) {
        float2 leftUV = leftRect.xy + float2(input.uv.x * 2.0, input.uv.y) * leftRect.zw;
        c = leftEye.Sample(samp, leftUV);
    } else {
        float2 rightUV = rightRect.xy + float2((input.uv.x - 0.5) * 2.0, input.uv.y) * rightRect.zw;
        c = rightEye.Sample(samp, rightUV);
    }
    return linearToSrgb(c);
}
)";