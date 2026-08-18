package com.google.cardboard.video;

import android.os.SystemClock;
import android.util.Log;
import java.util.function.Supplier;

/**
 * Re-broadcasts discovery whenever no video frame has arrived for a while.
 *
 * <p>The PC driver only streams to the phone after receiving a discovery packet, so if SteamVR
 * restarted (or the app reconnected to a different PC IP) the phone would otherwise sit with a dead
 * channel until the app restarts. This watchdog polls the decoder's last-frame timestamp and runs
 * the supplied reconnect action when the channel has been silent for {@link #STALL_MS}.
 *
 * <p>Extracted from {@link VideoManager} so the stall-detection policy is a standalone, testable
 * component. Runs on its own daemon thread; {@link #start()} and {@link #stop()} synchronize on an
 * internal lock to make start/stop idempotent.
 */
final class VideoWatchdog {
  // How long a silent video channel must be before we consider it stalled.
  private static final long STALL_MS = 3000;
  // Poll cadence for the decoder's last-frame timestamp.
  private static final long POLL_MS = 500;
  // Minimum gap between consecutive re-broadcast announcements while stalled.
  private static final long REANNOUNCE_INTERVAL_MS = 5000;

  private final String tag;
  private final Runnable reconnectAction;
  private final Supplier<VideoDecoder> decoderSupplier;

  private final Object lock = new Object();
  private Thread watchdogThread = null;
  private boolean running = false;

  VideoWatchdog(String tag, Runnable reconnectAction, Supplier<VideoDecoder> decoderSupplier) {
    this.tag = tag;
    this.reconnectAction = reconnectAction;
    this.decoderSupplier = decoderSupplier;
  }

  /** Start polling. Safe to call repeatedly; subsequent calls are ignored while running. */
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

  /** Stop polling (interrupts the daemon thread). Safe to call repeatedly. */
  void stop() {
    synchronized (lock) {
      running = false;
      if (watchdogThread != null) {
        watchdogThread.interrupt();
        watchdogThread = null;
      }
    }
  }

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
        // Re-broadcast at most every REANNOUNCE_INTERVAL_MS while the channel is dead.
        if (!wasStalled || now - lastAnnounceMs > REANNOUNCE_INTERVAL_MS) {
          Log.w(tag, "No video frames for >" + STALL_MS + "ms; re-broadcast discovery");
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