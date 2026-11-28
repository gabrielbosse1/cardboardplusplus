#pragma once
#include <cstdint>
// Pure encoder-geometry/settings validation shared by the driver's settings
// paths and the encoder unit tests. Keeping the rules in one header means the
// BRIDGE_CFG / CARDBOARD_CAP / defaults paths and the tests can never drift.
//
// H.264 (libx264 and every hardware encoder on the wire) encodes 16x16
// macroblocks: dimensions are rounded DOWN to multiples of 16. Note this means
// the classic 1920x1080 default becomes 1920x1072 (67 macroblock rows) — the
// CARDBOARD_CAP echo reports what actually runs.
namespace cbpp {

// Validates w/h against the 320x180..7680 range and rounds down to multiples
// of 16; false means "reject, caller keeps its current dimensions".
inline bool SanitizeEncoderDims(int& w, int& h)
{
    if (w < 320 || h < 180 || w > 7680 || h > 7680)
        return false;
    w = (w / 16) * 16;
    h = (h / 16) * 16;
    return w >= 320 && h >= 160;
}

// Result of validating one ApplyEncoderSettings request: the values to apply
// plus which inputs were rejected. Rejected fields keep the live values so a
// malformed datagram can never touch the running encoder's geometry.
struct ValidatedEncoderSettings {
    int width = 0, height = 0, fps = 0, bitrateBps = 0;
    bool dimsValid = false;       // false: caller keeps its current dims
    bool fpsAdjusted = false;     // input was outside 1..120
    bool bitrateAdjusted = false; // input was outside 1 kbps..100 Mbps (negative input = keep)
};

// Validates one settings request against the live values. fps outside 1..120
// and bitrate outside 1 kbps..100 Mbps fall back to the live values; a
// negative bitrateBps means "no change" (the BRIDGE_CFG path omits it).
inline ValidatedEncoderSettings ValidateEncoderSettings(int w, int h, int fps, int bitrateBps,
                                                        int curW, int curH, int curFps, int curBitrateBps)
{
    ValidatedEncoderSettings v;
    v.width = w;
    v.height = h;
    v.dimsValid = SanitizeEncoderDims(v.width, v.height);
    if (!v.dimsValid) {
        v.width = curW;
        v.height = curH;
    }
    v.fps = (fps >= 1 && fps <= 120) ? fps : curFps;
    v.fpsAdjusted = (v.fps != fps);
    if (bitrateBps >= 1000 && bitrateBps <= 100 * 1000 * 1000) {
        v.bitrateBps = bitrateBps;
        v.bitrateAdjusted = false;
    } else {
        v.bitrateBps = curBitrateBps;
        v.bitrateAdjusted = (bitrateBps >= 0);
    }
    return v;
}

}  // namespace cbpp
