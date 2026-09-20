package com.google.cardboard.video;
import android.os.SystemClock;
import android.util.Log;
import com.google.cardboard.core.DebugLog;
import java.util.function.Supplier;
// Stall monitor in video/; VideoManager starts it alongside playback to re-poke discovery on freeze.
final class VideoWatchdog {
  private static final DebugLog DBG = new DebugLog("VideoWatchdog");
  private static final long STALL_MS = 3000;
  private static final long POLL_MS = 500;
  private static final long REANNOUNCE_INTERVAL_MS = 5000;
  private final String tag;
  private final Runnable reconnectAction;
  private final Supplier<VideoDecoder> decoderSupplier;
  private final Object lock = new Object();
  private Thread watchdogThread = null;
  private boolean running = false;
  // Stores the log tag, reconnect hook, and decoder source; called from VideoManager.startWatchdog.
  VideoWatchdog(String tag, Runnable reconnectAction, Supplier<VideoDecoder> decoderSupplier) {
    this.tag = tag;
    this.reconnectAction = reconnectAction;
    this.decoderSupplier = decoderSupplier;
  }
  // Starts the stall-poll thread; called from VideoManager.start on the GL thread.
  void start() {
    synchronized (lock) {
      if (running || watchdogThread != null) {
        return;
      }
      running = true;
      watchdogThread = new Thread(this::pollLoop);
      watchdogThread.setDaemon(true);
      watchdogThread.start();
    }
  }
  // Stops the poll thread; called from VideoManager.onPause.
  void stop() {
    synchronized (lock) {
      running = false;
      if (watchdogThread != null) {
        watchdogThread.interrupt();
        watchdogThread = null;
      }
    }
  }
  // Polls the decoder timestamp and fires the reconnect hook on stall; runs on the watchdog thread.
  private void pollLoop() {
    long lastAnnounceMs = 0;
    long lastFrameSeenAt = 0;
    boolean wasStalled = false;
    while (running) {
      try {
        Thread.sleep(POLL_MS);
      } catch (InterruptedException e) {
        break;
      }
      VideoDecoder decoder = decoderSupplier.get();
      if (decoder == null) {
        continue;
      }
      long last = decoder.getLastFrameAtMs();
      if (last > lastFrameSeenAt) {
        lastFrameSeenAt = last;
      }
      boolean stalled =
          lastFrameSeenAt > 0 && (SystemClock.elapsedRealtime() - lastFrameSeenAt) > STALL_MS;
      if (stalled) {
        long now = SystemClock.elapsedRealtime();
        if (!wasStalled || now - lastAnnounceMs > REANNOUNCE_INTERVAL_MS) {
          DBG.w("No video frames for >" + STALL_MS + "ms; re-broadcast discovery");
          lastAnnounceMs = now;
          if (reconnectAction != null) {
            reconnectAction.run();
          }
        }
      }
      wasStalled = stalled;
    }
  }
}