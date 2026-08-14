package com.google.cardboard.video;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.SystemClock;
import android.util.Log;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.settings.AppSettings;
import com.google.cardboard.util.NetworkUtils;
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
  private final AppSettings appSettings;
  private VideoDecoder decoder;
  private boolean surfaceCreated = false;

  // Re-broadcast CARDBOARD_DISCOVERY to re-point the PC's video target if the
  // stream stalls (e.g. SteamVR restarted while the phone is still running).
  // The PC only forwards video after it hears a discovery packet, so a double
  // failure (PC restart + reappearing target reset to 127.0.0.1) leaves the
  // phone with no stream and no way to reconnect until the app restarts.
  private Runnable reconnectAction;
  private final Object watchdogLock = new Object();
  private Thread watchdogThread = null;
  private boolean watchdogRunning = false;

  public VideoManager(NativeBridge bridge, AppSettings appSettings) {
    this.bridge = bridge;
    this.appSettings = appSettings;
  }

  /** Called by the activity when the user re-runs discovery (e.g. after a PC restart). */
  public void setReconnectAction(Runnable reconnectAction) {
    this.reconnectAction = reconnectAction;
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
    startWatchdog();
  }

  /**
   * Re-broadcast discovery whenever no video frame has arrived for a while. The PC only streams to
   * the phone after receiving a discovery packet, so if SteamVR restarted (or the app reconnected
   * to a different PC IP), the phone would otherwise sit with a dead channel until the app restarts.
   */
  private void startWatchdog() {
    synchronized (watchdogLock) {
      if (watchdogRunning || watchdogThread != null) {
        return;
      }
      watchdogRunning = true;
      watchdogThread =
          new Thread(
              () -> {
                final long STALL_MS = 3000;
                final long POLL_MS = 500;
                long lastAnnounceMs = 0;
                long lastFrameSeenAt = 0;
                boolean wasStalled = false;
                while (watchdogRunning) {
                  try {
                    Thread.sleep(POLL_MS);
                  } catch (InterruptedException e) {
                    break;
                  }
                  VideoDecoder d = decoder;
                  if (d == null) {
                    continue;
                  }
                  long last = d.getLastFrameAtMs();
                  if (last > lastFrameSeenAt) {
                    lastFrameSeenAt = last;
                  }
                  boolean stalled = lastFrameSeenAt > 0
                      && (SystemClock.elapsedRealtime() - lastFrameSeenAt) > STALL_MS;
                  if (stalled) {
                    // Re-broadcast at most every 5s while the channel is dead.
                    long now = SystemClock.elapsedRealtime();
                    if (!wasStalled || now - lastAnnounceMs > 5000) {
                      Log.w(TAG, "No video frames for >" + STALL_MS + "ms; re-broadcast discovery");
                      lastAnnounceMs = now;
                      if (reconnectAction != null) {
                        reconnectAction.run();
                      }
                    }
                  }
                  wasStalled = stalled;
                }
              });
      watchdogThread.setDaemon(true);
      watchdogThread.start();
    }
  }

  private void stopWatchdog() {
    synchronized (watchdogLock) {
      watchdogRunning = false;
      if (watchdogThread != null) {
        watchdogThread.interrupt();
        watchdogThread = null;
      }
    }
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

  /** Send the decoder cap to the PC over the discovery UDP channel (broadcast or direct IP). */
  private void sendCapToPc(int width, int height) {
    new Thread(
            () -> {
              try (DatagramSocket socket = new DatagramSocket()) {
                socket.setBroadcast(true);
                InetAddress addr = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
                String msg = "CARDBOARD_CAP " + width + " " + height;
                byte[] data = msg.getBytes();
                // Send a few times in case the PC isn't listening yet.
                for (int i = 0; i < 3; i++) {
                  socket.send(
                      new DatagramPacket(data, data.length, addr, AppConstants.UDP_DISCOVERY_PORT));
                  Thread.sleep(500);
                }
                Log.i(TAG, "Sent decoder cap to PC (" + addr.getHostAddress() + "): " + msg);
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
    stopWatchdog();
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
