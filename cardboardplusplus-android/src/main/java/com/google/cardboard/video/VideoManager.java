package com.google.cardboard.video;

import android.util.Log;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.settings.AppSettings;

/**
 * Owns the video decode pipeline: creates the OES texture + MediaCodec-backed {@link VideoDecoder}
 * on the GL thread, starts the native UDP receiver that forwards H.264 access units to the decoder,
 * and drives {@link VideoDecoder#updateVideoTexture()} each frame.
 */
public class VideoManager {
  private static final String TAG = VideoManager.class.getSimpleName();

  private final NativeBridge bridge;
  private final DecoderCapabilityReporter capabilityReporter;
  private VideoDecoder decoder;
  private boolean surfaceCreated = false;

  // If the video stream stalls (e.g. SteamVR restarted behind the running phone),
  // re-broadcast discovery so the PC driver re-routes video to this phone.
  private Runnable reconnectAction;
  private VideoWatchdog watchdog;

  public VideoManager(NativeBridge bridge, AppSettings appSettings) {
    this.bridge = bridge;
    this.capabilityReporter = new DecoderCapabilityReporter(TAG, appSettings);
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
    int[] cap = capabilityReporter.queryDecoderCapability();
    Log.i(TAG, "Hardware decoder cap: " + cap[0] + "x" + cap[1]);
    capabilityReporter.sendCapToPc(cap[0], cap[1]);
    startWatchdog();
  }

  /**
   * Starts the stall watchdog that re-runs discovery when the video channel goes dead.
   *
   * <p>See {@link VideoWatchdog} for why this is necessary and how the re-broadcast is throttled.
   */
  private void startWatchdog() {
    if (watchdog == null) {
      watchdog = new VideoWatchdog(TAG, reconnectAction, () -> decoder);
    }
    watchdog.start();
  }

  private void stopWatchdog() {
    if (watchdog != null) {
      watchdog.stop();
    }
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
