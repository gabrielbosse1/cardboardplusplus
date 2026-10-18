// Fullscreen triangle vertex shader for the compose pass. Shared by the Windows
// runtime compiler and the Linux build, which extracts this raw string to HLSL and
// runs dxc at build time.
static const char* kVkBlitVSSource = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 xy = float2((id << 1) & 2, id & 2);
    o.pos = float4(xy * float2(2, 2) + float2(-1, -1), 0, 1);
    o.uv = xy;
    return o;
}
)";