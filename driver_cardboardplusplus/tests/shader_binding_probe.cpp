// TEMP diagnostic probe: compiles the actual driver HLSL shaders with
// d3dcompiler, translates them with the vendored dxbc-spirv library exactly as
// the driver does at runtime, and writes the raw SPIR-V to <name>.spv files.
// The CI workflow parses those files in PowerShell (crash-proof) and reports
// the emitted descriptor bindings, to verify whether BasicResourceMapping's
// sequential binding assignment matches the Vulkan descriptor layout used by
// VulkanCompose (compose PS/VS: 0=UBO, 1=sampler, 2/3=sampled images;
// NV12 CS: 0=UBO, 1=storage buffer).
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

static unsigned long g_sehCode = 0;

// Objects with destructors live here; the SEH wrapper must stay object-free.
static void RunStage(const char* name, const void* dxbc, size_t dxbcSize) {
    std::vector<uint32_t> words;
    bool ok = spirv::TranslateToSpirv(dxbc, dxbcSize, name, words);
    printf("== %s: translate ok=%d words=%zu\n", name, ok ? 1 : 0, words.size());
    if (!ok) return;
    char path[64];
    snprintf(path, sizeof(path), "%s.spv", name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        printf("== %s: fopen_s %s failed\n", name, path);
        return;
    }
    fwrite(words.data(), 4, words.size(), f);
    fclose(f);
    printf("== %s: wrote %s (%zu words)\n", name, path, words.size());
}

static void RunStageGuarded(const char* name, const void* dxbc, size_t dxbcSize) {
    __try {
        RunStage(name, dxbc, dxbcSize);
    } __except (g_sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        printf("== %s: EXCEPTION 0x%08lx during translate/write\n", name, g_sehCode);
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
                                "main", s.target, D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &err);
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
