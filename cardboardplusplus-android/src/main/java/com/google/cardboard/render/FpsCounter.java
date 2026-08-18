package com.google.cardboard.render;

import android.util.Log;

/**
 * Counts rendered frames and logs the render rate once per second.
 *
 * <p>Extracted from {@link VrRenderer} so the renderer's per-frame work is purely "do the next
 * frame" and diagnostics like this live in a self-contained object. The log tag is supplied by the
 * caller so existing log output stays unchanged.
 */
final class FpsCounter {
  private final String tag;

  private long frameCount;
  private long lastLogNs;

  FpsCounter(String tag) {
    this.tag = tag;
  }

  /** Account for one rendered frame; logs FPS when a full second has elapsed. */
  void onFrameRendered() {
    frameCount++;
    long now = System.nanoTime();
    if (lastLogNs == 0) {
      lastLogNs = now;
    } else if (now - lastLogNs >= 1_000_000_000L) {
      double fps = frameCount * 1e9 / (now - lastLogNs);
      Log.i(tag, "FPS=" + fps + " frames=" + frameCount);
      frameCount = 0;
      lastLogNs = now;
    }
  }
}