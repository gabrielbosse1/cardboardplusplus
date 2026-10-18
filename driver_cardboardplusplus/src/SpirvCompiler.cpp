#include "SpirvCompiler.h"

#include <optional>

#include "DriverLog.h"
#include "dxbc/dxbc_api.h"
#include "spirv/spirv_builder.h"

namespace spirv {

bool TranslateToSpirv(const void* bytecode, size_t byteSize, const char* debugName,
                      std::vector<uint32_t>& outWords) {
  auto irOpt = dxbc_spv::dxbc::compileShaderToLegalizedIr(bytecode, byteSize, {}, {});
  if (!irOpt) {
    DriverLog("SPIRV: IR conversion failed for %s", debugName ? debugName : "?");
    return false;
  }

  dxbc_spv::spirv::BasicResourceMapping mapping;
  dxbc_spv::spirv::SpirvBuilder sb(*irOpt, mapping, {});
  sb.buildSpirvBinary();
  outWords = sb.getSpirvBinary();

  if (outWords.size() <= 5 || outWords[0] != 0x07230203) {
    DriverLog("SPIRV: invalid SPIR-V from %s (%zu words)", debugName ? debugName : "?", outWords.size());
    return false;
  }
  // SpirvBuilder targets SPIR-V 1.6, which needs a Vulkan 1.3 driver. Every
  // capability it emits (VulkanMemoryModel, PhysicalStorageBufferAddresses,
  // DrawParameters) is core in 1.5, so retarget the header to run on 1.2 drivers.
  outWords[1] = 0x00010500;
  DriverLog("SPIRV: %s compiled %zu bytes -> %zu words", debugName ? debugName : "?", byteSize, outWords.size());
  return true;
}

}  // namespace spirv