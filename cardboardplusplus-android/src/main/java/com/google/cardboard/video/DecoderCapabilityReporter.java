package com.google.cardboard.video;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
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
  // Wire protocol prefix (shared with the driver) - format: "CARDBOARD_CAP W H".
  private static final String CAP_MESSAGE_PREFIX = "CARDBOARD_CAP ";
  // The PC may not be listening yet; sending a few times avoids dropping the cap notice.
  private static final int CAP_SEND_ATTEMPTS = 3;
  private static final long CAP_SEND_GAP_MS = 500;

  private final String tag;
  private final AppSettings appSettings;

  DecoderCapabilityReporter(String tag, AppSettings appSettings) {
    this.tag = tag;
    this.appSettings = appSettings;
  }

  /**
   * Query the AVC hardware decoder's supported width/height upper bounds. Returns the maximum
   * resolution the decoder can handle (used to clamp the encoder on the PC side).
   */
  int[] queryDecoderCapability() {
    int maxW = 1920;
    int maxH = 1920;
    MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
    for (MediaCodecInfo info : list.getCodecInfos()) {
      if (info.isEncoder() || !Arrays.asList(info.getSupportedTypes()).contains("video/avc")) {
        continue;
      }
      try {
        android.media.MediaCodecInfo.CodecCapabilities caps =
            info.getCapabilitiesForType("video/avc");
        MediaCodecInfo.VideoCapabilities vc = caps.getVideoCapabilities();
        if (vc != null) {
          maxW = (int) vc.getSupportedWidths().getUpper();
          maxH = (int) vc.getSupportedHeights().getUpper();
        }
      } catch (Exception e) {
        Log.w(tag, "Failed to query decoder cap for " + info.getName(), e);
      }
      break;
    }
    return new int[] {maxW, maxH};
  }

  /** Send the decoder cap to the PC over the discovery UDP channel (broadcast or direct IP). */
  void sendCapToPc(int width, int height) {
    new Thread(
            () -> {
              try (DatagramSocket socket = new DatagramSocket()) {
                socket.setBroadcast(true);
                InetAddress addr = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
                String msg = CAP_MESSAGE_PREFIX + width + " " + height;
                byte[] data = msg.getBytes();
                // Send a few times in case the PC isn't listening yet.
                for (int i = 0; i < CAP_SEND_ATTEMPTS; i++) {
                  socket.send(
                      new DatagramPacket(data, data.length, addr, AppConstants.UDP_DISCOVERY_PORT));
                  Thread.sleep(CAP_SEND_GAP_MS);
                }
                Log.i(tag, "Sent decoder cap to PC (" + addr.getHostAddress() + "): " + msg);
              } catch (Exception e) {
                Log.w(tag, "Failed to send decoder cap to PC", e);
              }
            })
        .start();
  }
}