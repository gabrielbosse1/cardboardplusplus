// BGRA -> NV12 (BT.601 limited range, full-chroma averaging) matching what
// swscale produces for the CPU path, so the phone sees the same colors.
// Each thread packs 8 pixels into 2 Y words + 2 UV words through a uint buffer,
// because RWByteAddressBuffer.Store would write 32 bits per store and make
// neighbouring pixel writes overlap.
static const char* kNv12CSSource = R"(
Texture2D<float4> srcTex : register(t0);
cbuffer Params : register(b0) { int4 dims; };  // x=width y=height z=uv plane byte offset
RWStructuredBuffer<uint> outBuf : register(u0);

float RgbToY(float3 c) {
    return 16.0f + 65.481f * c.r + 128.553f * c.g + 24.966f * c.b;
}
float2 RgbToUv(float3 c) {
    return float2(128.0f - 37.797f * c.r - 74.203f * c.g + 112.0f * c.b,
                  128.0f + 112.0f * c.r - 93.786f * c.g - 18.214f * c.b);
}
float2 ChromaAt(int cx, int cy) {
    int w = dims.x;
    int h = dims.y;
    float2 sum = float2(0.0f, 0.0f);
    [unroll]
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int x = min(cx * 2 + i, w - 1);
            int y = min(cy * 2 + j, h - 1);
            sum += RgbToUv(srcTex.Load(int3(x, y, 0)).rgb);
        }
    }
    return sum * 0.25f;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    int w = dims.x;
    int h = dims.y;
    int y = (int)tid.y;
    int xBase = (int)tid.x * 8;
    if (y >= h || xBase >= w) return;

    uint yBytes[8];
    [unroll]
    for (int i = 0; i < 8; i++) {
        int x = min(xBase + i, w - 1);
        float3 c = srcTex.Load(int3(x, y, 0)).rgb;
        yBytes[i] = (uint)clamp(RgbToY(c), 0.0f, 255.0f);
    }
    uint yWord0 = (uint)(y * (w >> 2) + (xBase >> 2));
    outBuf[yWord0] = yBytes[0] | (yBytes[1] << 8) | (yBytes[2] << 16) | (yBytes[3] << 24);
    if (xBase + 4 < w) {
        outBuf[yWord0 + 1] = yBytes[4] | (yBytes[5] << 8) | (yBytes[6] << 16) | (yBytes[7] << 24);
    }

    if (((y & 1) == 0) && (xBase & 7) == 0) {
        // 8 pixels wide = 4 chroma samples = 8 chroma bytes = 2 words.
        float2 c0 = ChromaAt((xBase >> 1), y >> 1);
        float2 c1 = ChromaAt((xBase >> 1) + 1, y >> 1);
        float2 c2 = ChromaAt((xBase >> 1) + 2, y >> 1);
        float2 c3 = ChromaAt((xBase >> 1) + 3, y >> 1);
        uint uvWord = (uint)((dims.z >> 2) + ((y >> 1) * (w >> 3)) + (xBase >> 2));
        outBuf[uvWord] = (uint)clamp(c0.x, 0.0f, 255.0f) |
                         ((uint)clamp(c0.y, 0.0f, 255.0f) << 8) |
                         ((uint)clamp(c1.x, 0.0f, 255.0f) << 16) |
                         ((uint)clamp(c1.y, 0.0f, 255.0f) << 24);
        outBuf[uvWord + 1] = (uint)clamp(c2.x, 0.0f, 255.0f) |
                             ((uint)clamp(c2.y, 0.0f, 255.0f) << 8) |
                             ((uint)clamp(c3.x, 0.0f, 255.0f) << 16) |
                             ((uint)clamp(c3.y, 0.0f, 255.0f) << 24);
    }
}
)";