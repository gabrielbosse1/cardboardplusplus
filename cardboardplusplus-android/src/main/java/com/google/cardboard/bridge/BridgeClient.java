package com.google.cardboard.bridge;

import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import com.google.cardboard.core.AppConstants;
import com.google.cardboard.settings.AppSettings;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.Socket;

/**
 * TCP client that connects to the PC bridge (port 42072).
 *
 * Protocol (newline-delimited text):
 *   Phone -> Bridge: REPORT_FPS fps=X 1low=Y
 *   Bridge -> Phone: SET_IPD X / GET_GYRO / PING
 *
 * The phone reports its FPS + 1% low every second.
 * Bridge detects disconnect when the TCP socket closes.
 */
public class BridgeClient {
    private static final String TAG = "BridgeClient";
    private static final long REPORT_INTERVAL_MS = 1000;

    public interface Callback {
        void onConnected();
        void onDisconnected();
        void onCommand(String cmd);
    }

    private final AppSettings appSettings;
    private final FpsTracker fpsTracker;
    private Callback callback;
    private HandlerThread reportThread;
    private Handler reportHandler;
    private Thread readThread;
    private volatile boolean running;
    private volatile boolean connected;
    private Socket socket;
    private OutputStream outputStream;
    private BufferedReader reader;

    public BridgeClient(AppSettings appSettings) {
        this.appSettings = appSettings;
        this.fpsTracker = new FpsTracker();
    }

    public void setCallback(Callback callback) {
        this.callback = callback;
    }

    /** Call this from the GL render loop to track frame times. */
    public void onFrame() {
        fpsTracker.onFrame();
    }

    public void start() {
        if (running) return;
        running = true;
        reportThread = new HandlerThread("BridgeClient-Report");
        reportThread.start();
        reportHandler = new Handler(reportThread.getLooper());
        new Thread(this::run, "BridgeClient-Connect").start();
    }

    public void stop() {
        running = false;
        closeSocket();
        if (reportThread != null) {
            reportThread.quitSafely();
            reportThread = null;
        }
        if (readThread != null) {
            readThread.interrupt();
            readThread = null;
        }
    }

    public boolean isConnected() {
        return connected;
    }

    private void run() {
        while (running) {
            try {
                connect();
                if (!running) break;
                // readLoop blocks, so run on its own thread
                readThread = new Thread(this::readLoop, "BridgeClient-Read");
                readThread.start();
                // Schedule periodic FPS reports on the handler thread
                reportHandler.post(this::reportFpsLoop);
                // Wait for read thread to finish (disconnect)
                readThread.join();
                reportHandler.removeCallbacksAndMessages(null);
            } catch (Exception e) {
                Log.w(TAG, "[BRIDGE] Connection error: " + e.getMessage());
            } finally {
                closeSocket();
                if (connected) {
                    connected = false;
                    Log.i(TAG, "[BRIDGE] Disconnected from bridge");
                    Callback cb = callback;
                    if (cb != null) cb.onDisconnected();
                }
            }
            if (running) {
                try { Thread.sleep(3000); } catch (InterruptedException ignored) { break; }
            }
        }
    }

    private void connect() throws Exception {
        String pcIp = appSettings.getPcIp();
        if (pcIp == null || pcIp.isEmpty()) {
            Log.w(TAG, "[BRIDGE] No PC IP configured, cannot connect");
            throw new Exception("No PC IP configured");
        }

        Log.i(TAG, "[BRIDGE] Connecting to bridge at " + pcIp + ":" + AppConstants.BRIDGE_TCP_PORT);
        socket = new Socket(pcIp, AppConstants.BRIDGE_TCP_PORT);
        socket.setTcpNoDelay(true);
        outputStream = socket.getOutputStream();
        reader = new BufferedReader(new InputStreamReader(socket.getInputStream()));
        connected = true;
        Log.i(TAG, "[BRIDGE] Connected to bridge");
        Callback cb = callback;
        if (cb != null) cb.onConnected();
    }

    private void readLoop() {
        try {
            String line;
            while (running && (line = reader.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                Log.d(TAG, "[BRIDGE] recv: " + line);
                Callback cb = callback;
                if (cb != null) cb.onCommand(line);
            }
        } catch (Exception e) {
            if (running) {
                Log.w(TAG, "[BRIDGE] Read error: " + e.getMessage());
            }
        }
    }

    private void reportFpsLoop() {
        if (!connected || !running) return;
        reportFps();
        if (connected && running) {
            reportHandler.postDelayed(this::reportFpsLoop, REPORT_INTERVAL_MS);
        }
    }

    private void reportFps() {
        if (!connected || outputStream == null) return;
        try {
            FpsTracker.Result result = fpsTracker.getResult();
            String msg = String.format(java.util.Locale.US,
                    "REPORT_FPS fps=%.1f 1low=%.1f\n", result.fps, result.onePercentLow);
            outputStream.write(msg.getBytes());
            outputStream.flush();
            Log.d(TAG, "[BRIDGE] send: " + msg.trim());
        } catch (Exception e) {
            Log.w(TAG, "[BRIDGE] Send error: " + e.getMessage());
        }
    }

    /** Send a command string to the bridge. */
    public void sendCommand(String cmd) {
        if (!connected || outputStream == null) return;
        try {
            outputStream.write((cmd + "\n").getBytes());
            outputStream.flush();
            Log.d(TAG, "[BRIDGE] send: " + cmd);
        } catch (Exception e) {
            Log.w(TAG, "[BRIDGE] Send error: " + e.getMessage());
        }
    }

    private void closeSocket() {
        try { if (reader != null) reader.close(); } catch (Exception ignored) {}
        try { if (outputStream != null) outputStream.close(); } catch (Exception ignored) {}
        try { if (socket != null) socket.close(); } catch (Exception ignored) {}
        socket = null;
        outputStream = null;
        reader = null;
    }

    /**
     * Tracks frame times and computes FPS + 1% low.
     * Uses a ring buffer of the last 120 frame timestamps.
     */
    public static class FpsTracker {
        private static final int BUFFER_SIZE = 120;
        private final long[] frameTimes = new long[BUFFER_SIZE];
        private int head = 0;
        private int count = 0;
        private long lastFrameNs = 0;

        public synchronized void onFrame() {
            long now = System.nanoTime();
            if (lastFrameNs != 0) {
                frameTimes[head] = now - lastFrameNs;
                head = (head + 1) % BUFFER_SIZE;
                if (count < BUFFER_SIZE) count++;
            }
            lastFrameNs = now;
        }

        public synchronized Result getResult() {
            if (count < 2) return new Result(0, 0);

            long totalNs = 0;
            for (int i = 0; i < count; i++) totalNs += frameTimes[i];
            double fps = count * 1e9 / totalNs;

            long[] sorted = new long[count];
            System.arraycopy(frameTimes, 0, sorted, 0, count);
            java.util.Arrays.sort(sorted);
            int cutoffIdx = Math.max(1, (int)(count * 0.99));
            double onePercentLow = 1e9 / sorted[cutoffIdx];

            return new Result(fps, onePercentLow);
        }

        public static class Result {
            public final double fps;
            public final double onePercentLow;

            public Result(double fps, double onePercentLow) {
                this.fps = fps;
                this.onePercentLow = onePercentLow;
            }
        }
    }
}
