// TEMP diagnostic probe: compiles the actual driver HLSL shaders with
// d3dcompiler, translates them with the vendored dxbc-spirv library exactly as
// the driver does at runtime, and dumps the emitted SPIR-V descriptor
// bindings. Verifies whether BasicResourceMapping's sequential binding
// assignment matches the Vulkan descriptor layout used by VulkanCompose
// (compose PS/VS: 0=UBO, 1=sampler, 2/3=sampled images; NV12 CS: 0=UBO,
// 1=storage buffer).
// Each stage runs under an SEH guard so one faulting shader cannot block the
// rest; the faulting scan offset is reported.
#include "SpirvCompiler.h"
#include "DriverLog.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <vector>
#include <windows.h>
#include "shaders/compose_vs.hlsli"
#include "shaders/compose_ps.hlsli"
#include "shaders/nv12_cs.hlsli"

#pragma comment(lib, "d3dcompiler.lib")

// Stub for the linker: TranslateToSpirv logs via DriverLog, which in the real
// driver goes through SteamVR's initialized context. In this probe there is no
// context, and the real DriverLog.cpp would crash on VRDriverLog(); print to
// stdout instead (unbuffered main keeps output alive through crashes).
void DriverLog(const char* pFormat, ...) {
    va_list args;
    va_start(args, pFormat);
    vprintf(pFormat, args);
    va_end(args);
    printf("\n");
}

static size_t g_scanOff = 0;

void ScanSpirv(const char* name, const std::vector<uint32_t>& words) {
    printf("== %s: %zu words, version=0x%08x\n", name, words.size(),
           words.size() > 1 ? words[1] : 0);
    auto at = [&words](size_t idx) -> uint32_t {
        return idx < words.size() ? words[idx] : 0;
    };
    size_t i = 5;
    while (i + 1 < words.size()) {
        g_scanOff = i;
        uint32_t w = at(i);
        uint32_t wc = w >> 16, op = w & 0xFFFF;
        if (wc == 0) { printf("  (zero word count at %zu)\n", i); break; }
        if (i + wc > words.size()) { printf("  (truncated instruction at %zu)\n", i); break; }
        if (op == 71 && wc >= 3) {  // OpDecorate target decoration [literal]
            uint32_t target = at(i + 1), dec = at(i + 2);
            uint32_t lit = (wc >= 4) ? at(i + 3) : 0;
            if (dec == 34) printf("  Decorate id=%u Binding=%u\n", target, lit);
            if (dec == 35) printf("  Decorate id=%u DescriptorSet=%u\n", target, lit);
        } else if (op == 5 && wc >= 2) {  // OpName target name...
            uint32_t target = at(i + 1);
            char nm[80];
            size_t n = 0;
            for (size_t j = i + 2; j < i + wc && n < sizeof(nm) - 1; j++) {
                uint32_t w4 = at(j);
                for (int b = 0; b < 4 && n < sizeof(nm) - 1; b++) {
                    char c = (char)((w4 >> (8 * b)) & 0xFF);
                    if (c == '\0') { j = i + wc; break; }
                    nm[n++] = c;
                }
            }
            nm[n] = '\0';
            printf("  Name id=%u \"%s\"\n", target, nm);
        }
        i += wc;
    }
    printf("== %s: scan complete\n", name);
}

static unsigned long g_sehCode = 0;

// Objects with destructors live here; the SEH wrapper must stay object-free.
static void RunStage(const char* name, const void* dxbc, size_t dxbcSize) {
    std::vector<uint32_t> words;
    bool ok = spirv::TranslateToSpirv(dxbc, dxbcSize, name, words);
    printf("== %s: translate ok=%d words=%zu\n", name, ok ? 1 : 0, words.size());
    if (ok) ScanSpirv(name, words);
}

static void RunStageGuarded(const char* name, const void* dxbc, size_t dxbcSize) {
    __try {
        RunStage(name, dxbc, dxbcSize);
    } __except (g_sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        printf("== %s: EXCEPTION 0x%08lx (scan offset %zu)\n", name, g_sehCode,
               g_scanOff);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // survive crashes without losing output
    printf("PROBE START\n");
    struct Shader {
        const char* name;
        const char* src;
        const char* target;
    } shaders[] = {
        {"compose_ps", kVkSBSPSSource, "ps_5_0"},
        {"nv12_cs", kNv12CSSource, "cs_5_0"},
        {"compose_vs", kVkBlitVSSource, "vs_5_0"},
    };
    for (const Shader& s : shaders) {
        printf("== %s: before D3DCompile\n", s.name);
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(s.src, strlen(s.src), s.name, nullptr, nullptr,
                                "main", s.target, 0, 0, &code, &err);
        if (FAILED(hr)) {
            printf("== %s: D3DCompile failed hr=0x%08lx\n%s\n", s.name,
                   (unsigned long)hr,
                   err ? (const char*)err->GetBufferPointer() : "");
            if (err) err->Release();
            continue;
        }
        printf("== %s: DXBC %zu bytes\n", s.name, code->GetBufferSize());
        RunStageGuarded(s.name, code->GetBufferPointer(), code->GetBufferSize());
        code->Release();
        printf("== %s: stage done\n", s.name);
    }
    printf("PROBE DONE\n");
    return 0;
}
