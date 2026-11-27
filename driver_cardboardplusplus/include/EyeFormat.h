#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>
#ifdef _WIN32
#include <dxgiformat.h>
#endif

namespace cbpp {
// OpenVR's Windows direct-mode SwapTextureSetDesc_t::nFormat is a DXGI_FORMAT:
// https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md
// These typed color formats retain their component order, bit width and transfer
// function when imported into Vulkan. Linux receives VkFormat values directly and
// must not use this conversion. Typeless, depth, compressed and X-channel formats
// are rejected rather than guessed; external-memory support is device-dependent.
enum class EyeDxgiFormat : uint32_t {
    R32G32B32A32Float = 2,
    R16G16B16A16Float = 10,
    R10G10B10A2Unorm = 24,
    R11G11B10Float = 26,
    R8G8B8A8Unorm = 28,
    R8G8B8A8Srgb = 29,
    R16G16Float = 34,
    R16Float = 54,
    R16Unorm = 56,
    R8Unorm = 61,
    B8G8R8A8Unorm = 87,
    B8G8R8A8Srgb = 91,
};

#ifdef _WIN32
// Verify the portable numeric values against the Windows SDK used by the build.
#define CBPP_CHECK_DXGI(name, sdkName) \
    static_assert(static_cast<uint32_t>(EyeDxgiFormat::name) == DXGI_FORMAT_##sdkName, "DXGI format mismatch")
CBPP_CHECK_DXGI(R32G32B32A32Float, R32G32B32A32_FLOAT);
CBPP_CHECK_DXGI(R16G16B16A16Float, R16G16B16A16_FLOAT);
CBPP_CHECK_DXGI(R10G10B10A2Unorm, R10G10B10A2_UNORM);
CBPP_CHECK_DXGI(R11G11B10Float, R11G11B10_FLOAT);
CBPP_CHECK_DXGI(R8G8B8A8Unorm, R8G8B8A8_UNORM);
CBPP_CHECK_DXGI(R8G8B8A8Srgb, R8G8B8A8_UNORM_SRGB);
CBPP_CHECK_DXGI(R16G16Float, R16G16_FLOAT);
CBPP_CHECK_DXGI(R16Float, R16_FLOAT);
CBPP_CHECK_DXGI(R16Unorm, R16_UNORM);
CBPP_CHECK_DXGI(R8Unorm, R8_UNORM);
CBPP_CHECK_DXGI(B8G8R8A8Unorm, B8G8R8A8_UNORM);
CBPP_CHECK_DXGI(B8G8R8A8Srgb, B8G8R8A8_UNORM_SRGB);
#undef CBPP_CHECK_DXGI
#endif

inline VkFormat EyeDxgiToVk(uint32_t format) {
    switch (static_cast<EyeDxgiFormat>(format)) {
        case EyeDxgiFormat::R32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case EyeDxgiFormat::R16G16B16A16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
        // DXGI packs R into the least significant bits, matching A2B10G10R10.
        case EyeDxgiFormat::R10G10B10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case EyeDxgiFormat::R11G11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case EyeDxgiFormat::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case EyeDxgiFormat::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case EyeDxgiFormat::R16G16Float: return VK_FORMAT_R16G16_SFLOAT;
        case EyeDxgiFormat::R16Float: return VK_FORMAT_R16_SFLOAT;
        case EyeDxgiFormat::R16Unorm: return VK_FORMAT_R16_UNORM;
        case EyeDxgiFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
        case EyeDxgiFormat::B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case EyeDxgiFormat::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
        default: return VK_FORMAT_UNDEFINED;
    }
}
}  // namespace cbpp
