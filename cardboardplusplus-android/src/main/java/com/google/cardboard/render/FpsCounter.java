package com.google.cardboard.render;

import com.google.cardboard.core.DebugLog;

/**
 * Counts rendered frames and logs the render rate once per second.
 *
 * <p>Extracted from {@link VrRenderer} so the renderer's per-frame work is purely "do the next
 * frame" and diagnostics like this live in a self-contained object. The log tag is supplied by the
 * caller so existing log output stays unchanged. Logging is gated on the debug
 * flag (VrActivity owns the global) so release sessions stay quiet.
 */
final class FpsCounter {
  private final DebugLog dbg;

  private long frameCount;
  private long lastLogNs;

  FpsCounter(String tag) {
    this.dbg = new DebugLog(tag);
  }

  /** Account for one rendered frame; logs FPS when a full second has elapsed. */
  void onFrameRendered() {
    frameCount++;
    long now = System.nanoTime();
    if (lastLogNs == 0) {
      lastLogNs = now;
    } else if (now - lastLogNs >= 1_000_000_000L) {
      double fps = frameCount * 1e9 / (now - lastLogNs);
      dbg.i("FPS=" + fps + " frames=" + frameCount);
      frameCount = 0;
      lastLogNs = now;
    }
  }
}