package com.google.cardboard.video;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.util.Log;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * Owns the video decode pipeline: creates the OES texture + MediaCodec-backed {@link VideoDecoder}
 * on the GL thread, starts the native UDP receiver that forwards H.264 access units to the decoder,
 * and drives {@link VideoDecoder#updateVideoTexture()} each frame.
 */
public class VideoManager {
  private static final String TAG = VideoManager.class.getSimpleName();

  private final NativeBridge bridge;
  private VideoDecoder decoder;
  private boolean surfaceCreated = false;

  public VideoManager(NativeBridge bridge) {
    this.bridge = bridge;
  }

  /** Create the OES texture and MediaCodec decoder. Must run on the GL thread. */
  public void onSurfaceCreated() {
    int texId = bridge.createVideoTexture();
    decoder =
        new VideoDecoder(
            bridge, texId, AppConstants.DEFAULT_VIDEO_WIDTH, AppConstants.DEFAULT_VIDEO_HEIGHT);
    bridge.setVideoDecoder(decoder);
    surfaceCreated = true;
    Log.i(TAG, "Video decoder created (tex=" + texId + ")");
  }

  public void start() {
    if (!surfaceCreated) {
      Log.w(TAG, "start() called before surface created; ignoring");
      return;
    }
    bridge.startVideoReceiver(AppConstants.VIDEO_PORT);
    Log.i(TAG, "Video receiver started on port " + AppConstants.VIDEO_PORT);
    // Report this device's hardware decoder cap to the PC so the encoder can be
    // clamped to what the decoder actually supports.
    int[] cap = queryDecoderCap();
    Log.i(TAG, "Hardware decoder cap: " + cap[0] + "x" + cap[1]);
    sendCapToPc(cap[0], cap[1]);
  }

  /**
   * Query the AVC hardware decoder's supported width/height upper bounds. Returns the maximum
   * resolution the decoder can handle (used to clamp the encoder on the PC side).
   */
  private int[] queryDecoderCap() {
    int maxW = 1920;
    int maxH = 1920;
    MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
    for (MediaCodecInfo info : list.getCodecInfos()) {
      if (info.isEncoder() || !java.util.Arrays.asList(info.getSupportedTypes()).contains("video/avc")) {
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
        Log.w(TAG, "Failed to query decoder cap for " + info.getName(), e);
      }
      break;
    }
    return new int[] {maxW, maxH};
  }

  /** Broadcast the decoder cap to the PC over the discovery UDP channel. */
  private void sendCapToPc(int width, int height) {
    new Thread(
            () -> {
              try (DatagramSocket socket = new DatagramSocket()) {
                socket.setBroadcast(true);
                InetAddress addr = InetAddress.getByName("255.255.255.255");
                String msg = "CARDBOARD_CAP " + width + " " + height;
                byte[] data = msg.getBytes();
                // Send a few times in case the PC isn't listening yet.
                for (int i = 0; i < 3; i++) {
                  socket.send(
                      new DatagramPacket(data, data.length, addr, AppConstants.UDP_DISCOVERY_PORT));
                  Thread.sleep(500);
                }
                Log.i(TAG, "Sent decoder cap to PC: " + msg);
              } catch (Exception e) {
                Log.w(TAG, "Failed to send decoder cap to PC", e);
              }
            })
        .start();
  }

  /** Present the latest decoded frame into the GL OES texture. Call from the GL thread. */
  public void updateTexture() {
    if (decoder != null) {
      decoder.updateVideoTexture();
    }
  }

  /** Tear down the decoder and stop the receiver. Call on pause. */
  public void onPause() {
    if (decoder != null) {
      decoder.release();
      decoder = null;
    }
    bridge.stopVideoReceiver();
    surfaceCreated = false;
  }

  public boolean isStarted() {
    return surfaceCreated;
  }
}
