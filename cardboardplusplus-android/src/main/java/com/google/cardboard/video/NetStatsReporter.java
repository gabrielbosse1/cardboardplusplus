package com.google.cardboard.video;

import android.os.SystemClock;
import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.function.Supplier;

/**
 * Reports video-path health to the bridge so it can adapt the encoder.
 *
 * <p>Every {@link #REPORT_INTERVAL_MS} the reporter samples the decoder (frames decoded since the
 * last report, time since the last frame) and sends one tag-{@code 0x13} datagram to the bridge
 * telemetry port (UDP 42071):
 * <pre>
 *   [0x13][u64 timestamp_ms LE][u32 frames decoded LE][u32 stalls LE][f32 decoded fps LE]
 * </pre>
 * 21 bytes total. The video stream itself is the load probe — arrival rate, stalls and decode
 * rate measure the actual driver→phone path, which no generic speed test can see.
 */
public class NetStatsReporter {
  private static final String TAG = NetStatsReporter.class.getSimpleName();

  static final byte NET_STATS_TAG = 0x13;
  static final int NET_STATS_LEN = 21;
  static final long REPORT_INTERVAL_MS = 2000;
  static final long STALL_MS = 3000;

  private final AppSettings appSettings;
  private final Supplier<VideoDecoder> decoderSupplier;

  private Thread reportThread = null;
  private volatile boolean running = false;

  private int lastTotalFrames = 0;
  private int stalls = 0;
  private boolean wasStalled = false;

  public NetStatsReporter(AppSettings appSettings, Supplier<VideoDecoder> decoderSupplier) {
    this.appSettings = appSettings;
    this.decoderSupplier = decoderSupplier;
  }

  /** Pure packet builder (no Android deps) so unit tests can verify the wire format. */
  public static byte[] buildPacket(long timestampMs, int framesDecoded, int stalls, float decodedFps) {
    ByteBuffer buf = ByteBuffer.allocate(NET_STATS_LEN).order(ByteOrder.LITTLE_ENDIAN);
    buf.put(NET_STATS_TAG);
    buf.putLong(timestampMs);
    buf.putInt(framesDecoded);
    buf.putInt(stalls);
    buf.putFloat(decodedFps);
    return buf.array();
  }

  public void start() {
    if (running || (reportThread != null && reportThread.isAlive())) {
      return;
    }
    running = true;
    reportThread = new Thread(this::reportLoop, "netstats-reporter");
    reportThread.setDaemon(true);
    reportThread.start();
  }

  public void stop() {
    running = false;
    if (reportThread != null) {
      reportThread.interrupt();
      reportThread = null;
    }
  }

  private void reportLoop() {
    while (running) {
      try {
        Thread.sleep(REPORT_INTERVAL_MS);
      } catch (InterruptedException e) {
        break;
      }
      try {
        sendReport();
      } catch (Exception e) {
        Log.w(TAG, "Net stats report failed: " + e.getMessage());
      }
    }
  }

  private void sendReport() throws Exception {
    VideoDecoder decoder = decoderSupplier != null ? decoderSupplier.get() : null;
    int frames = 0;
    float fps = 0f;
    if (decoder != null) {
      int total = decoder.getTotalDecodedFrames();
      frames = total - lastTotalFrames;
      lastTotalFrames = total;
      fps = frames * 1000f / REPORT_INTERVAL_MS;
      long last = decoder.getLastFrameAtMs();
      boolean stalled = last > 0 && (SystemClock.elapsedRealtime() - last) > STALL_MS;
      if (stalled && !wasStalled) {
        stalls++;
      }
      wasStalled = stalled;
    }
    byte[] data = buildPacket(System.currentTimeMillis(), frames, stalls, fps);
    InetAddress addr = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
    try (DatagramSocket socket = new DatagramSocket()) {
      socket.send(new DatagramPacket(data, data.length, addr, AppConstants.TELEMETRY_PORT));
    }
  }
}
