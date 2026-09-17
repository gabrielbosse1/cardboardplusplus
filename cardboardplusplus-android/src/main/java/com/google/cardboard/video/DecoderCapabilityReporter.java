package com.google.cardboard.video;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
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
   * <p>Only hardware-accelerated decoders are considered. Software decoders may advertise higher
   * resolution caps (e.g. 2048x2048) but the actual hardware decoder used at runtime has a lower
   * ceiling (e.g. 1920x1920). Reporting the software cap causes the PC encoder to produce a
   * stream the hardware decoder can't handle.
   */
  static int[] queryDecoderCapability() {
    int maxW = 1920;
    int maxH = 1920;
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
        android.media.MediaCodecInfo.CodecCapabilities caps =
            info.getCapabilitiesForType("video/avc");
        MediaCodecInfo.VideoCapabilities vc = caps.getVideoCapabilities();
        if (vc != null) {
          int w = (int) vc.getSupportedWidths().getUpper();
          int h = (int) vc.getSupportedHeights().getUpper();
          Log.i("DecoderCap", "HW decoder " + info.getName() + " caps: " + w + "x" + h);
          if (w > maxW) maxW = w;
          if (h > maxH) maxH = h;
        }
      } catch (Exception e) {
        Log.w("DecoderCap", "Failed to query decoder cap for " + info.getName(), e);
      }
    }
    Log.i("DecoderCap", "Final HW decoder cap: " + maxW + "x" + maxH);
    return new int[] {maxW, maxH};
  }
}