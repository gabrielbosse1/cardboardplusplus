package com.google.cardboard.video;
import android.os.SystemClock;
import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import com.google.cardboard.telemetry.TelemetrySender;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.function.Supplier;
// Decoder health reporter in video/; VideoManager runs it to send 0x13 packets for bridge bitrate control.
public class NetStatsReporter {
  private static final String TAG = NetStatsReporter.class.getSimpleName();
  static final byte NET_STATS_TAG = AppConstants.TELEMETRY_TAG_NETSTATS;
  static final int NET_STATS_LEN = 21;
  static final long REPORT_INTERVAL_MS = 2000;
  static final long STALL_MS = 3000;
  private final AppSettings appSettings;
  private final Supplier<VideoDecoder> decoderSupplier;
  private final TelemetrySender telemetrySender;
  private Thread reportThread = null;
  private volatile boolean running = false;
  private int lastTotalFrames = 0;
  private int stalls = 0;
  private boolean wasStalled = false;
  private VideoDecoder lastDecoder;
  // Stores settings, decoder source, and optional telemetry path; called from VideoManager.startNetStats.
  public NetStatsReporter(AppSettings appSettings, Supplier<VideoDecoder> decoderSupplier) {
    this(appSettings, decoderSupplier, null);
  }
  // Stores settings, decoder source, and telemetry sender; called from VideoManager.startNetStats.
  public NetStatsReporter(
      AppSettings appSettings, Supplier<VideoDecoder> decoderSupplier, TelemetrySender telemetrySender) {
    this.appSettings = appSettings;
    this.decoderSupplier = decoderSupplier;
    this.telemetrySender = telemetrySender;
  }
  // Encodes one 21-byte 0x13 packet; called from sendReport on the reporter thread.
  public static byte[] buildPacket(long timestampMs, int framesDecoded, int stalls, float decodedFps) {
    ByteBuffer buf = ByteBuffer.allocate(NET_STATS_LEN).order(ByteOrder.LITTLE_ENDIAN);
    buf.put(NET_STATS_TAG);
    buf.putLong(timestampMs);
    buf.putInt(framesDecoded);
    buf.putInt(stalls);
    buf.putFloat(decodedFps);
    return buf.array();
  }
  // Starts the 2s report loop; called from VideoManager.start on the GL thread.
  public void start() {
    if (running || (reportThread != null && reportThread.isAlive())) {
      return;
    }
    running = true;
    reportThread = new Thread(this::reportLoop, "netstats-reporter");
    reportThread.setDaemon(true);
    reportThread.start();
  }
  // Stops the report loop; called from VideoManager.onPause on the UI thread.
  public void stop() {
    running = false;
    if (reportThread != null) {
      reportThread.interrupt();
      reportThread = null;
    }
  }
  // Sleeps 2s between reports and sends each sample; runs on the netstats reporter thread.
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
  // Samples decoder counters and sends one packet; called from reportLoop on the reporter thread.
  private void sendReport() throws Exception {
    VideoDecoder decoder = decoderSupplier != null ? decoderSupplier.get() : null;
    int frames = 0;
    float fps = 0f;
    if (decoder != null) {
      if (decoder != lastDecoder) {
        lastDecoder = decoder;
        lastTotalFrames = 0;
        wasStalled = false;
      }
      int total = decoder.getTotalDecodedFrames();
      frames = total - lastTotalFrames;
      if (frames < 0) frames = total;
      lastTotalFrames = total;
      fps = frames * 1000f / REPORT_INTERVAL_MS;
      long last = decoder.getLastFrameAtMs();
      boolean stalled = last > 0 && (SystemClock.elapsedRealtime() - last) > STALL_MS;
      if (stalled && !wasStalled) {
        stalls++;
      }
      wasStalled = stalled;
    }
    byte[] data = buildPacket(SystemClock.elapsedRealtime(), frames, stalls, fps);
    if (telemetrySender != null && telemetrySender.sendDatagram(data)) {
      return;
    }
    InetAddress addr = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
    try (DatagramSocket socket = new DatagramSocket()) {
      socket.send(new DatagramPacket(data, data.length, addr, AppConstants.TELEMETRY_PORT));
    }
  }
}
