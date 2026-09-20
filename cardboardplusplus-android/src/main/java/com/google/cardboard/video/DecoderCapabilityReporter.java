package com.google.cardboard.video;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.util.Log;
import java.util.Arrays;
// Hardware decode ceiling probe in video/; VideoManager queries it to feed DiscoveryManager CAP reports.
final class DecoderCapabilityReporter {
  private DecoderCapabilityReporter() {}
  // Returns the max decode size of the selected or smallest HW decoder; called from VideoManager.queryDecoderCap.
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
  // Returns the cap of the format-selected HW decoder; called from queryDecoderCapability.
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
      Log.w("DecoderCap", "Selected decoder " + name + " is not HW AVC; using HW fallback");
      return null;
    } catch (Exception e) {
      Log.w("DecoderCap", "Selected-decoder lookup failed; using HW fallback", e);
      return null;
    }
  }
  // Returns the smallest-area HW decoder cap with 1920 fallback; called from queryDecoderCapability.
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
  // Reads the width and height ceilings of one decoder; called from both cap queries.
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
