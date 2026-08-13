package com.google.cardboard.video;

import android.util.Log;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;

/** Wraps the native SBS video receiver (start / per-frame update / stop). */
public class VideoManager {
  private static final String TAG = VideoManager.class.getSimpleName();

  private final NativeBridge bridge;
  private boolean videoReceiverStarted = false;

  public VideoManager(NativeBridge bridge) {
    this.bridge = bridge;
  }

  /** Starts the receiver exactly once. Safe to call repeatedly. */
  public void start() {
    if (!videoReceiverStarted) {
      bridge.startVideoReceiver(AppConstants.VIDEO_PORT);
      videoReceiverStarted = true;
      Log.i(TAG, "Video receiver started on port " + AppConstants.VIDEO_PORT);
    }
  }

  /** Pulls the latest decoded frame into the GL texture. Call from the GL thread. */
  public void updateTexture() {
    if (videoReceiverStarted) {
      bridge.updateVideoTexture();
    }
  }

  public void stop() {
    if (videoReceiverStarted) {
      bridge.stopVideoReceiver();
      videoReceiverStarted = false;
    }
  }

  public boolean isStarted() {
    return videoReceiverStarted;
  }
}
