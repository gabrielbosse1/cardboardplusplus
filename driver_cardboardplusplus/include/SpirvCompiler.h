#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spirv {

// Translates a compiled shader blob (DXIL on Windows) into SPIR-V words.
// debugName is only used for log lines on failure.
// Returns true and fills outWords on success.
bool TranslateToSpirv(const void* bytecode, size_t byteSize, const char* debugName,
                      std::vector<uint32_t>& outWords);

}  // namespace spirv