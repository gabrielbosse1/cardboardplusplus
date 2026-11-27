#pragma once
// Per-frame compositor layer staging shared between SubmitLayer and Present.
// SubmitLayer appends up to kMaxSubmittedLayers entries per frame; Present
// atomically drains or discards the staged batch so each frame starts empty.
#include <atomic>
#include <cstddef>
#include <vector>

namespace cbpp {

static const std::size_t kMaxSubmittedLayers = 4;

template <typename Layer>
inline bool StageSubmittedLayer(std::vector<Layer>& staged,
                                std::atomic<bool>& hasSubmit,
                                const Layer& layer,
                                std::size_t maxLayers = kMaxSubmittedLayers)
{
    if (staged.size() >= maxLayers)
        return false;
    staged.push_back(layer);
    hasSubmit.store(true, std::memory_order_release);
    return true;
}

template <typename Layer>
inline void DiscardSubmittedLayers(std::vector<Layer>& staged,
                                   std::atomic<bool>& hasSubmit)
{
    staged.clear();
    hasSubmit.store(false, std::memory_order_release);
}

template <typename Layer>
inline bool DrainSubmittedLayers(std::vector<Layer>& staged,
                                 std::atomic<bool>& hasSubmit,
                                 std::vector<Layer>& out)
{
    if (!hasSubmit.load(std::memory_order_acquire)) {
        staged.clear();
        return false;
    }
    out.swap(staged);
    staged.clear();
    hasSubmit.store(false, std::memory_order_release);
    return true;
}

} // namespace cbpp
