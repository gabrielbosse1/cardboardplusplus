package com.google.cardboard.video;
import android.os.SystemClock;
import android.util.Log;
import com.google.cardboard.core.DebugLog;
import java.util.function.Supplier;
// Stall monitor in video/; VideoManager starts it alongside playback. Two failure
// shapes are watched separately: no frames arriving at all (re-poke discovery),
// and frames arriving that the decoder never turns into output (reset the codec).
final class VideoWatchdog {
  private static final DebugLog DBG = new DebugLog("VideoWatchdog");
  private static final long STALL_MS = 3000;
  private static final long POLL_MS = 500;
  private static final long OUTPUT_STALL_MS = 1500;
  private static final long REANNOUNCE_INTERVAL_MS = 5000;
  private final String tag;
  private final Runnable reconnectAction;
  private final Runnable reconfigureAction;
  private final Supplier<VideoDecoder> decoderSupplier;
  private final Object lock = new Object();
  private Thread watchdogThread = null;
  private boolean running = false;
  // Stores the log tag, reconnect hook, decoder-reset hook, and decoder source;
  // called from VideoManager.startWatchdog.
  VideoWatchdog(
      String tag,
      Runnable reconnectAction,
      Runnable reconfigureAction,
      Supplier<VideoDecoder> decoderSupplier) {
    this.tag = tag;
    this.reconnectAction = reconnectAction;
    this.reconfigureAction = reconfigureAction;
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
  // Polls the decoder input and output counters and fires the recovery hooks;
  // runs on the watchdog thread.
  private void pollLoop() {
    long lastAnnounceMs = 0;
    long lastFrameSeenAt = 0;
    long lastOutputAtMs = 0;
    int lastDecodedFrames = -1;
    boolean wasStalled = false;
    boolean wasMuted = false;
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
      long now = SystemClock.elapsedRealtime();
      long last = decoder.getLastFrameAtMs();
      if (last > lastFrameSeenAt) {
        lastFrameSeenAt = last;
      }
      // Output progress is what the viewer sees: a codec that accepts frames but
      // renders none holds the picture still while every other signal looks fine.
      int decoded = decoder.getTotalDecodedFrames();
      if (lastDecodedFrames < 0 || decoded != lastDecodedFrames) {
        lastDecodedFrames = decoded;
        lastOutputAtMs = now;
      }
      boolean stalled =
          lastFrameSeenAt > 0 && (now - lastFrameSeenAt) > STALL_MS;
      if (stalled) {
        if (!wasStalled || now - lastAnnounceMs > REANNOUNCE_INTERVAL_MS) {
          DBG.w("No video frames for >" + STALL_MS + "ms; re-broadcast discovery");
          lastAnnounceMs = now;
          if (reconnectAction != null) {
            reconnectAction.run();
          }
        }
      }
      wasStalled = stalled;
      // Input is fresh but no frame has been decoded for OUTPUT_STALL_MS: the
      // codec is wedged. Rebuilding it costs a few hundred ms and un-freezes,
      // while leaving it alone keeps the last frame on screen indefinitely.
      boolean muted =
          lastFrameSeenAt > 0
              && (now - lastFrameSeenAt) <= STALL_MS
              && (now - lastOutputAtMs) > OUTPUT_STALL_MS;
      if (muted) {
        if (!wasMuted) {
          DBG.w("Frames arriving but none decoded for >" + OUTPUT_STALL_MS + "ms");
        }
        if (reconfigureAction != null) {
          reconfigureAction.run();
        }
      }
      wasMuted = muted;
    }
  }
}