#include "EyeFormat.h"
#include <cstdio>

// Tests the Windows DXGI-to-Vulkan import mapping without requiring a GPU.
int main() {
    struct Case { uint32_t dxgi; VkFormat vk; };
    const Case cases[] = {
        {2, VK_FORMAT_R32G32B32A32_SFLOAT},
        {10, VK_FORMAT_R16G16B16A16_SFLOAT},
        {24, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {26, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
        {28, VK_FORMAT_R8G8B8A8_UNORM},
        {29, VK_FORMAT_R8G8B8A8_SRGB},
        {34, VK_FORMAT_R16G16_SFLOAT},
        {54, VK_FORMAT_R16_SFLOAT},
        {56, VK_FORMAT_R16_UNORM},
        {61, VK_FORMAT_R8_UNORM},
        {87, VK_FORMAT_B8G8R8A8_UNORM},
        {91, VK_FORMAT_B8G8R8A8_SRGB},
        {0, VK_FORMAT_UNDEFINED},    // UNKNOWN
        {27, VK_FORMAT_UNDEFINED},   // R8G8B8A8_TYPELESS
        {45, VK_FORMAT_UNDEFINED},   // D24_UNORM_S8_UINT
        {88, VK_FORMAT_UNDEFINED},   // B8G8R8X8_UNORM
        {95, VK_FORMAT_UNDEFINED},   // BC6H_UF16
        {0xffffffffu, VK_FORMAT_UNDEFINED},
    };
    for (const Case& c : cases) {
        VkFormat actual = cbpp::EyeDxgiToVk(c.dxgi);
        if (actual != c.vk) {
            std::fprintf(stderr, "DXGI %u: expected Vulkan %d, got %d\n",
                         c.dxgi, static_cast<int>(c.vk), static_cast<int>(actual));
            return 1;
        }
    }
    std::puts("Eye format mappings passed");
    return 0;
}
