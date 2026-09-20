package com.google.cardboard.render;
import com.google.cardboard.core.DebugLog;
// Per-second GL frame-rate logger: counts onDrawFrame calls and emits one
// FPS line per second. Debug-gated via DebugLog; owned by VrRenderer.
final class FpsCounter {
  private final DebugLog dbg;
  private long frameCount;
  private long lastLogNs;
  // Binds the counter to the owner's log tag (VrRenderer's).
  FpsCounter(String tag) {
    this.dbg = new DebugLog(tag);
  }
  // Records one rendered frame; logs and resets the window each full second.
  // Called from VrRenderer.onDrawFrame on the GL thread.
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