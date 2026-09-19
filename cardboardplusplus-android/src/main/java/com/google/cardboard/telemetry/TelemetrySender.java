package com.google.cardboard.telemetry;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;
import com.google.cardboard.BuildConfig;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Reads sensors and sends telemetry packets to the bridge over UDP 42071.
 *
 * <p>Sends two packet types:
 * <ul>
 *   <li>Tag 0x10 (45 bytes): raw accel/gyro/mag for bridge display diagnostics,
 *       throttled to ~10Hz (the driver tracks via 0x12, not this)</li>
 *   <li>Tag 0x12 (25 bytes): fused rotation quaternion from
 *       TYPE_GAME_ROTATION_VECTOR for driver head tracking, full rate</li>
 * </ul>
 *
 * <p>The game rotation vector is Android's sensor fusion of accel+gyro (no
 * magnetometer) into an absolute orientation quaternion, converted to OpenVR
 * space. The driver uses this directly for SteamVR head tracking.
 *
 * <p>All sensor callbacks run on a dedicated HandlerThread (never the UI
 * thread); packets are sent inline on that thread with reused buffers, so the
 * hot path allocates nothing.
 */
public class TelemetrySender implements SensorEventListener {
    private static final String TAG = TelemetrySender.class.getSimpleName();
    private static final DebugLog DBG = new DebugLog(TAG);
    // Locked wire tags (centralized in AppConstants; values must not change).
    private static final byte GYRO_TAG = AppConstants.TELEMETRY_TAG_GYRO;
    private static final byte ROTATION_TAG = AppConstants.TELEMETRY_TAG_ROTATION;

    private final Context context;
    private final SensorManager sensorManager;
    private final AppSettings appSettings;

    // Written by connect thread, read by sensor thread — volatile for visibility.
    private volatile DatagramSocket socket;
    private volatile InetAddress bridgeAddress;
    private volatile boolean connected;

    // Sensor values — sensor thread only (registered with its handler).
    private final float[] accel = new float[3];
    private final float[] gyro = new float[3];
    private final float[] mag = new float[3];

    // Fused rotation quaternion from TYPE_ROTATION_VECTOR.
    private final float[] rotationQuat = new float[4]; // [w, x, y, z]

    // Throttle diagnostics-only raw packets to ~10Hz (rotation stays full rate).
    private long lastRawNs = 0;
    private static final long MIN_RAW_INTERVAL_NS = 100_000_000L; // 100 ms

    // Reused packet buffers + datagram (sensor thread only, no per-event alloc).
    private final ByteBuffer rawBuf = ByteBuffer.allocate(45).order(ByteOrder.LITTLE_ENDIAN);
    private final ByteBuffer rotBuf = ByteBuffer.allocate(25).order(ByteOrder.LITTLE_ENDIAN);
    private final DatagramPacket sendPacket = new DatagramPacket(new byte[0], 0);

    // Sensor thread: callbacks + UDP sends all happen here, off the UI thread.
    private HandlerThread sensorThread;
    private Handler sensorHandler;

    // Scratch for quaternion math (sensor thread only).
    private final float[] tmpA = new float[4];
    private final float[] tmpB = new float[4];

    // Cached display rotation (getSystemService+getRotation per packet is too
    // hot); refreshed at most once per second. No onConfigurationChanged hook
    // exists here, so a time-based refresh bounds the staleness instead.
    // getDefaultDisplay is deprecated since 30 but still works through target
    // 35; tablets pinned to ROTATION_0 just take the identity branch below.
    private int cachedRotation = Surface.ROTATION_0;
    private long lastRotCheckMs = 0;
    private static final long ROTATION_CACHE_MS = 1000;

    private volatile boolean running;

    public TelemetrySender(Context context, AppSettings appSettings) {
        this.context = context;
        this.sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.appSettings = appSettings;
    }

    public void start() {
        if (running) return;
        running = true;
        connected = false;

        sensorThread = new HandlerThread("TelemetrySensors");
        sensorThread.start();
        sensorHandler = new Handler(sensorThread.getLooper());

        Sensor accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        Sensor gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        Sensor magSensor = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD);
        // Debug: log all rotation vector sensors found.
        for (Sensor s : sensorManager.getSensorList(Sensor.TYPE_ROTATION_VECTOR)) {
            DBG.d("ROT_SENSOR avail: type=11 vendor=%s name=%s handle=%d", s.getVendor(), s.getName(), s.getName().hashCode());
        }
        for (Sensor s : sensorManager.getSensorList(Sensor.TYPE_GAME_ROTATION_VECTOR)) {
            DBG.d("GAME_SENSOR avail: type=15 vendor=%s name=%s handle=%d", s.getVendor(), s.getName(), s.getName().hashCode());
        }
        // Use the default/vendor Game Rotation Vector sensor (accel+gyro fusion).
        // Other VR apps use this and it fires at 200+ Hz. The AOSP variant is much slower.
        Sensor rotVecSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
        if (rotVecSensor == null) rotVecSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
        if (rotVecSensor != null) DBG.i("Using rotVecSensor: %s vendor=%s", rotVecSensor.getName(), rotVecSensor.getVendor());

        if (accelSensor == null) DBG.w("No accelerometer found");
        if (gyroSensor == null) DBG.w("No gyroscope found");
        if (magSensor == null) DBG.w("No magnetometer found");
        if (rotVecSensor == null) DBG.w("No rotation vector sensor found");

        int delay = SensorManager.SENSOR_DELAY_GAME;
        if (accelSensor != null) sensorManager.registerListener(this, accelSensor, delay, sensorHandler);
        if (gyroSensor != null) sensorManager.registerListener(this, gyroSensor, delay, sensorHandler);
        if (magSensor != null) sensorManager.registerListener(this, magSensor, delay, sensorHandler);
        // Register rotation vector sensor for fused orientation.
        if (rotVecSensor != null) sensorManager.registerListener(this, rotVecSensor, delay, sensorHandler);

        // Connect thread: creates socket, then the sensor thread uses it.
        Thread connectThread = new Thread(this::connectLoop, "telemetry-connect");
        connectThread.setDaemon(true);
        connectThread.start();

        Log.i(TAG, "TelemetrySender started ("
                + (accelSensor != null ? "accel" : "no-accel") + ", "
                + (gyroSensor != null ? "gyro" : "no-gyro") + ", "
                + (magSensor != null ? "mag" : "no-mag") + ", "
                + (rotVecSensor != null ? "rotVec" : "no-rotVec") + ")"
                + (DBG.isEnabled() ? " [DEBUG]" : ""));
    }

    public void stop() {
        running = false;
        connected = false;
        sensorManager.unregisterListener(this);
        DatagramSocket s = socket;
        if (s != null && !s.isClosed()) {
            s.close();
        }
        socket = null;
        bridgeAddress = null;
        if (sensorThread != null) {
            sensorThread.quitSafely();
            sensorThread = null;
        }
        sensorHandler = null;
        Log.i(TAG, "TelemetrySender stopped");
    }

    private void connectLoop() {
        while (running) {
            try {
                DatagramSocket s = new DatagramSocket();
                InetAddress addr = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
                // Do NOT connect – keep socket unconnected so we can send to any address
                // (connected + explicit destination causes EPERM on some Android versions).
                // Socket ready — publish to other threads via volatile writes.
                socket = s;
                bridgeAddress = addr;
                connected = true;  // volatile write AFTER socket/address — memory fence
                Log.i(TAG, "Telemetry connected to " + addr.getHostAddress() + ":" + AppConstants.TELEMETRY_PORT);
                // Send hello so bridge learns phone IP (was 0.0.0.0 before).
                try {
                    byte[] hello = phoneHelloBytes();
                    s.send(new DatagramPacket(hello, hello.length, addr, AppConstants.TELEMETRY_PORT));
                    DBG.d("Sent phone hello to %s:%d", addr.getHostAddress(), AppConstants.TELEMETRY_PORT);
        } catch (Exception e) {
          Log.w(TAG, "phone hello send failed", e);
        }

                // Keep socket alive; the sender thread uses it. Re-resolve the
                // destination while alive: the PC-IP setting or the network
                // (WiFi/Tailscale/DHCP) can change mid-session, and a pinned
                // address survives forever because UDP-to-blackhole never
                // throws (previously required an app restart to recover).
                String lastPcIp = appSettings.getPcIp();
                if (lastPcIp == null) lastPcIp = "";
                long lastResolveMs = System.currentTimeMillis();
                while (running && connected) {
                    Thread.sleep(500);
                    try {
                        String cur = appSettings.getPcIp();
                        if (cur == null) cur = "";
                        long now = System.currentTimeMillis();
                        if (!cur.equals(lastPcIp) || now - lastResolveMs > 5000) {
                            InetAddress fresh =
                                    NetworkUtils.getPcOrBroadcastAddress(cur);
                            lastPcIp = cur;
                            lastResolveMs = now;
                            if (!fresh.equals(bridgeAddress)) {
                                bridgeAddress = fresh;
                                Log.i(TAG, "Telemetry retargeted to "
                                        + fresh.getHostAddress() + ":"
                                        + AppConstants.TELEMETRY_PORT);
                                try {
                                    byte[] hello = phoneHelloBytes();
                                    s.send(new DatagramPacket(hello, hello.length,
                                            fresh, AppConstants.TELEMETRY_PORT));
                                } catch (Exception e) {
                                    Log.w(TAG, "retarget hello send failed", e);
                                }
                            }
                        }
                    } catch (Exception e) {
                        Log.w(TAG, "Telemetry retarget failed: "
                                + e.getClass().getSimpleName());
                    }
                }
            } catch (Exception e) {
                connected = false;
                Log.w(TAG, "Telemetry connection failed: " + e.getClass().getSimpleName() + ": " + e.getMessage());
                DBG.d("Telemetry reconnecting in 2s...");
            }
            if (!running) break;
            try { Thread.sleep(2000); } catch (InterruptedException ie) { break; }
        }
    }

    /**
     * Hello bytes with our build version appended ({@code CARDBOARD_PHONE_HELLO v1 <n>}).
     * Static so tests can verify the wire format without a Context.
     */
    static byte[] phoneHelloBytes() {
        return AppConstants.phoneHello(BuildConfig.VERSION_NAME).getBytes();
    }

    /**
     * Send an already-framed datagram (e.g. NetStats 0x13) on the telemetry
     * socket, reusing the cached bridge address. Returns false when no socket
     * is live so the caller can fall back to its own socket.
     */
    public boolean sendDatagram(byte[] data) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null || !connected) return false;
        try {
            // Fresh packet: called from the NetStats thread, not the sensor
            // thread that owns the reused sendPacket (every 2s, so the alloc
            // is negligible).
            s.send(new DatagramPacket(data, data.length, addr, AppConstants.TELEMETRY_PORT));
            return true;
        } catch (Exception e) {
            Log.w(TAG, "Shared telemetry send failed: " + e.getClass().getSimpleName());
            connected = false;
            return false;
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (!connected) return;

        switch (event.sensor.getType()) {
            case Sensor.TYPE_ACCELEROMETER:
                System.arraycopy(event.values, 0, accel, 0, 3);
                maybeSendRaw(event.timestamp);
                break;
            case Sensor.TYPE_GYROSCOPE:
                System.arraycopy(event.values, 0, gyro, 0, 3);
                maybeSendRaw(event.timestamp);
                break;
            case Sensor.TYPE_MAGNETIC_FIELD:
                System.arraycopy(event.values, 0, mag, 0, 3);
                maybeSendRaw(event.timestamp);
                break;
            case Sensor.TYPE_ROTATION_VECTOR:
            case Sensor.TYPE_GAME_ROTATION_VECTOR:
                handleRotationVector(event);
                break;
        }
    }

    // World-frame conversion: Android earth frame is Z-up, OpenVR is Y-up.
    // Rx(-90°) maps Z(up) → Y(up). Left-multiplied onto every sample.
    private static final float S = 0.70710678f;
    private static final float[] Q_WORLD = {S, -S, 0f, 0f};
    // Fixed mount correction: the viewer holds the phone 180° about the screen
    // normal from the pinned landscape frame, so roll the result 180° about Z.
    private static final float[] Q_ROLL180 = {0f, 0f, 0f, 1f};
    // Precomputed device-frame inverses per display rotation (no per-event alloc).
    // Indexed 0..3 for ROTATION_0/90/180/270 (Surface constants match indices).
    private static final float[][] Q_DEV_INV = {
        {1f, 0f, 0f, 0f},   // ROTATION_0
        {S, 0f, 0f, S},     // ROTATION_90
        {0f, 0f, 0f, -1f},  // ROTATION_180
        {S, 0f, 0f, -S},    // ROTATION_270
    };

    /** Quaternion multiply: out = a * b (all [w, x, y, z]). Writes into out, no alloc. */
    private static void qmulInto(float[] a, float[] b, float[] out) {
        out[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
        out[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
        out[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
        out[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
    }

    private void handleRotationVector(SensorEvent event) {
        float x = event.values[0], y = event.values[1], z = event.values[2];
        float w = event.values.length > 3 ? event.values[3]
                : (float) Math.sqrt(Math.max(0f, 1f - x * x - y * y - z * z));

        // Game rotation vector gives device→world in the portrait device frame
        // with a Z-up earth frame. OpenVR wants device→world in the mounted
        // (VR) device frame with a Y-up world frame:
        //   q_openvr = Q_WORLD * q_raw * q_device⁻¹ * Q_ROLL180
        // The result is absolute (gravity-level pitch/roll), so the bridge and
        // driver use it directly — no reference capture, no recenter needed.
        tmpB[0] = w; tmpB[1] = x; tmpB[2] = y; tmpB[3] = z;
        qmulInto(Q_WORLD, tmpB, tmpA);
        int rot = getDisplayRotation();
        qmulInto(tmpA, Q_DEV_INV[rot & 3], tmpB);
        qmulInto(tmpB, Q_ROLL180, tmpA);
        rotationQuat[0] = tmpA[0];
        rotationQuat[1] = tmpA[1];
        rotationQuat[2] = tmpA[2];
        rotationQuat[3] = tmpA[3];
        sendRotationPacket(event.timestamp);
    }

    private void maybeSendRaw(long timestampNs) {
        if (timestampNs - lastRawNs < MIN_RAW_INTERVAL_NS) return;
        lastRawNs = timestampNs;
        sendRawPacket(timestampNs);
    }

    /** Send raw accel/gyro/mag (tag 0x10) for driver gyro fallback and bridge diagnostics.
     *  Values are remapped from the phone's natural portrait frame to VR/landscape frame
     *  based on the current display rotation, so the driver's fallback pitch/roll/yaw
     *  formulas (which assume VR coordinates) work correctly. */
    private void sendRawPacket(long timestampNs) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null) return;

        long timestampMs = timestampNs / 1_000_000;

        rawBuf.clear();
        rawBuf.put(GYRO_TAG);
        rawBuf.putLong(timestampMs);
        // Remap portrait→VR inline (no temp arrays):
        // ROTATION_90: VR_X=P_Y, VR_Y=-P_X; ROTATION_270: VR_X=-P_Y, VR_Y=P_X.
        int rot = getDisplayRotation();
        if (rot == Surface.ROTATION_90) {
            putVec3(rawBuf, gyro[1], -gyro[0], gyro[2]);
            putVec3(rawBuf, accel[1], -accel[0], accel[2]);
            putVec3(rawBuf, mag[1], -mag[0], mag[2]);
        } else if (rot == Surface.ROTATION_270) {
            putVec3(rawBuf, -gyro[1], gyro[0], gyro[2]);
            putVec3(rawBuf, -accel[1], accel[0], accel[2]);
            putVec3(rawBuf, -mag[1], mag[0], mag[2]);
        } else {
            putVec3(rawBuf, gyro[0], gyro[1], gyro[2]);
            putVec3(rawBuf, accel[0], accel[1], accel[2]);
            putVec3(rawBuf, mag[0], mag[1], mag[2]);
        }

        try {
            sendPacket.setData(rawBuf.array(), 0, rawBuf.position());
            sendPacket.setAddress(addr);
            sendPacket.setPort(AppConstants.TELEMETRY_PORT);
            s.send(sendPacket);
        } catch (Exception e) {
            Log.w(TAG, "Telemetry send failed: " + e.getClass().getSimpleName() + ": " + e.getMessage());
            connected = false;
        }
    }

    private static void putVec3(ByteBuffer buf, float x, float y, float z) {
        buf.putFloat(x);
        buf.putFloat(y);
        buf.putFloat(z);
    }

    /** Send fused rotation quaternion (tag 0x12) for driver head tracking. */
    private void sendRotationPacket(long timestampNs) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null) return;

        long timestampMs = timestampNs / 1_000_000;

        rotBuf.clear();
        rotBuf.put(ROTATION_TAG);
        rotBuf.putLong(timestampMs);
        for (float v : rotationQuat) rotBuf.putFloat(v); // w, x, y, z

        try {
            sendPacket.setData(rotBuf.array(), 0, rotBuf.position());
            sendPacket.setAddress(addr);
            sendPacket.setPort(AppConstants.TELEMETRY_PORT);
            s.send(sendPacket);
        } catch (Exception e) {
            Log.w(TAG, "Rotation send failed: " + e.getClass().getSimpleName() + ": " + e.getMessage());
            connected = false;
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    private int getDisplayRotation() {
        long now = System.currentTimeMillis();
        if (now - lastRotCheckMs < ROTATION_CACHE_MS) return cachedRotation;
        lastRotCheckMs = now;
        try {
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            if (wm != null) {
                Display display = wm.getDefaultDisplay();
                cachedRotation = display.getRotation();
            }
        } catch (Exception ignored) {}
        return cachedRotation;
    }
}
