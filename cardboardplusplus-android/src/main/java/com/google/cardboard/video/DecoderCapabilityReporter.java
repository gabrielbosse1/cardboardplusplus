package com.google.cardboard.video;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.util.Log;
import java.util.Arrays;

/**
 * Discovers the device's AVC hardware-decoder capability ceiling and announces it to the PC.
 *
 * <p>Extracted from {@link VideoManager} so the video pipeline owner only orchestrates the decoder,
 * receiver and watchdog while the "tell the PC what my decoder can actually handle" concern lives
 * here.
 *
 * <p>The announced message ({@code CARDBOARD_CAP <width> <height>}) and the discovery UDP port are
 * part of the runtime protocol shared with the PC driver and must not change.
 */
final class DecoderCapabilityReporter {
  private DecoderCapabilityReporter() {}

  /**
   * Query the AVC hardware decoder's supported width/height upper bounds. Returns the maximum
   * resolution the decoder can handle (used to clamp the encoder on the PC side).
   *
   * <p>Reports the cap of the decoder MediaCodec will actually select (resolved by
   * name via {@code findDecoderForFormat}), so a thumbnail secondary decoder can
   * neither drag the ceiling down nor let a software decoder inflate it past
   * what the hardware player handles. Falls back to the smallest-area
   * hardware AVC decoder. Width and height always come from the same decoder —
   * never mixed across decoders.
   */
  static int[] queryDecoderCapability() {
    int[] selected = querySelectedDecoderCap();
    if (selected != null) {
      Log.i("DecoderCap", "Selected decoder cap: " + selected[0] + "x" + selected[1]);
      return selected;
    }
    int[] fallback = queryMinAreaHardwareCap();
    Log.i("DecoderCap", "Final HW decoder cap: " + fallback[0] + "x" + fallback[1]);
    return fallback;
  }

  /** Cap of the decoder MediaCodec would select for our AVC stream, if it is hardware. */
  private static int[] querySelectedDecoderCap() {
    try {
      MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
      MediaFormat format = MediaFormat.createVideoFormat(
          "video/avc",
          com.google.cardboard.core.AppConstants.DEFAULT_VIDEO_WIDTH,
          com.google.cardboard.core.AppConstants.DEFAULT_VIDEO_HEIGHT);
      String name = list.findDecoderForFormat(format);
      if (name == null) return null;
      for (MediaCodecInfo info : list.getCodecInfos()) {
        if (!info.getName().equals(name) || info.isEncoder()
            || !Arrays.asList(info.getSupportedTypes()).contains("video/avc")
            || !info.isHardwareAccelerated()) {
          continue;
        }
        return capOf(info);
      }
      // MediaCodec would select a software decoder: its cap is not the
      // hardware ceiling, so fall back to the hardware min-area instead.
      Log.w("DecoderCap", "Selected decoder " + name + " is not HW AVC; using HW fallback");
      return null;
    } catch (Exception e) {
      Log.w("DecoderCap", "Selected-decoder lookup failed; using HW fallback", e);
      return null;
    }
  }

  /** Smallest-area hardware AVC decoder cap (W+H from that one decoder). */
  private static int[] queryMinAreaHardwareCap() {
    int bestW = 1920;
    int bestH = 1920;
    long bestArea = (long) bestW * bestH;
    boolean found = false;
    MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
    for (MediaCodecInfo info : list.getCodecInfos()) {
      if (info.isEncoder() || !Arrays.asList(info.getSupportedTypes()).contains("video/avc")) {
        continue;
      }
      // Skip software decoders — only the hardware decoder's limits matter because
      // that's the one Android's MediaCodec selector will pick at runtime.
      if (!info.isHardwareAccelerated()) {
        continue;
      }
      try {
        int[] cap = capOf(info);
        if (cap == null) continue;
        Log.i("DecoderCap", "HW decoder " + info.getName() + " caps: " + cap[0] + "x" + cap[1]);
        long area = (long) cap[0] * cap[1];
        if (!found || area < bestArea) {
          bestW = cap[0];
          bestH = cap[1];
          bestArea = area;
          found = true;
        }
      } catch (Exception e) {
        Log.w("DecoderCap", "Failed to query decoder cap for " + info.getName(), e);
      }
    }
    return new int[] {bestW, bestH};
  }

  /** Per-decoder upper bounds (W+H from the same decoder, never mixed). */
  private static int[] capOf(MediaCodecInfo info) {
    android.media.MediaCodecInfo.CodecCapabilities caps =
        info.getCapabilitiesForType("video/avc");
    MediaCodecInfo.VideoCapabilities vc = caps.getVideoCapabilities();
    if (vc == null) return null;
    return new int[] {
      (int) vc.getSupportedWidths().getUpper(),
      (int) vc.getSupportedHeights().getUpper(),
    };
  }
}
