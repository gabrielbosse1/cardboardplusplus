package com.google.cardboard.telemetry;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Reads sensors and sends telemetry packets to the bridge over UDP 42071.
 *
 * <p>Sends two packet types:
 * <ul>
 *   <li>Tag 0x10 (45 bytes): raw accel/gyro/mag for bridge display diagnostics</li>
 *   <li>Tag 0x12 (25 bytes): fused rotation quaternion from
 *       TYPE_GAME_ROTATION_VECTOR for driver head tracking</li>
 * </ul>
 *
 * <p>The game rotation vector is Android's sensor fusion of accel+gyro (no
 * magnetometer) into an absolute orientation quaternion, converted to OpenVR
 * space. The driver uses this directly for SteamVR head tracking.
 */
public class TelemetrySender implements SensorEventListener {
    private static final String TAG = TelemetrySender.class.getSimpleName();
    private static final DebugLog DBG = DebugLog.create(TelemetrySender.class, null);
    private static final byte GYRO_TAG = 0x10;
    private static final byte ROTATION_TAG = 0x12;

    private final Context context;
    private final SensorManager sensorManager;
    private final AppSettings appSettings;

    // Written by connect thread, read by sensor callback — volatile for visibility.
    private volatile DatagramSocket socket;
    private volatile InetAddress bridgeAddress;
    private volatile boolean connected;

    // Sensor values — written by sensor callback, read by sender thread.
    private final float[] accel = new float[3];
    private final float[] gyro = new float[3];
    private final float[] mag = new float[3];

    // Fused rotation quaternion from TYPE_ROTATION_VECTOR.
    private final float[] rotationQuat = new float[4]; // [w, x, y, z]

    // Throttle raw packets to ~200 Hz max (sensor delivers at ~200 Hz GAME).
    private long lastRawNs = 0;
    private static final long MIN_RAW_INTERVAL_NS = 5_000_000L; // 5 ms

    private ExecutorService executor = Executors.newSingleThreadExecutor();

    private volatile boolean running;

    public TelemetrySender(Context context, AppSettings appSettings) {
        this.context = context;
        this.sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.appSettings = appSettings;
        DBG.setEnabled(appSettings.isDebugLogging());
    }

    public void start() {
        if (running) return;
        // stop() shuts the executor down; recreate it so pause/resume works.
        if (executor == null || executor.isShutdown() || executor.isTerminated()) {
            executor = Executors.newSingleThreadExecutor();
        }
        running = true;
        connected = false;

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
        if (accelSensor != null) sensorManager.registerListener(this, accelSensor, delay);
        if (gyroSensor != null) sensorManager.registerListener(this, gyroSensor, delay);
        if (magSensor != null) sensorManager.registerListener(this, magSensor, delay);
        // Register rotation vector sensor for fused orientation.
        if (rotVecSensor != null) sensorManager.registerListener(this, rotVecSensor, delay);

        // Connect thread: creates socket, then sender thread uses it.
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
        executor.shutdownNow();
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
                    byte[] hello = "CARDBOARD_PHONE_HELLO v1".getBytes();
                    s.send(new DatagramPacket(hello, hello.length, addr, AppConstants.TELEMETRY_PORT));
                    DBG.d("Sent phone hello to %s:%d", addr.getHostAddress(), AppConstants.TELEMETRY_PORT);
        } catch (Exception e) {
          Log.w(TAG, "phone hello send failed", e);
        }

                // Keep socket alive; the sender thread uses it.
                while (running && connected) {
                    Thread.sleep(500);
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

    /** Quaternion multiply: out = a * b (all [w, x, y, z]). */
    private static float[] qmul(float[] a, float[] b) {
        return new float[]{
            a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3],
            a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2],
            a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1],
            a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0],
        };
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
        float[] q1 = qmul(Q_WORLD, new float[]{w, x, y, z});
        float[] qDevInv;
        int rot = getDisplayRotation();
        if (rot == Surface.ROTATION_90) {
            qDevInv = new float[]{S, 0f, 0f, S};
        } else if (rot == Surface.ROTATION_270) {
            qDevInv = new float[]{S, 0f, 0f, -S};
        } else if (rot == Surface.ROTATION_180) {
            qDevInv = new float[]{0f, 0f, 0f, -1f};
        } else {
            qDevInv = new float[]{1f, 0f, 0f, 0f};
        }
        float[] out = qmul(qmul(q1, qDevInv), Q_ROLL180);
        rotationQuat[0] = out[0];
        rotationQuat[1] = out[1];
        rotationQuat[2] = out[2];
        rotationQuat[3] = out[3];
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

        // Remap accel/gyro/mag from portrait to VR frame based on display rotation.
        // Portrait: X-right, Y-up, Z-toward-user.
        // VR (ROTATION_90): VR_X=Portrait_Y, VR_Y=-Portrait_X, VR_Z=Portrait_Z.
        // VR (ROTATION_270): VR_X=-Portrait_Y, VR_Y=Portrait_X, VR_Z=Portrait_Z.
        float[] ga = gyro, aa = accel, ma = mag;
        int rot = getDisplayRotation();
        if (rot == Surface.ROTATION_90) {
            ga = new float[]{ gyro[1], -gyro[0], gyro[2] };
            aa = new float[]{ accel[1], -accel[0], accel[2] };
            ma = new float[]{ mag[1], -mag[0], mag[2] };
        } else if (rot == Surface.ROTATION_270) {
            ga = new float[]{ -gyro[1], gyro[0], gyro[2] };
            aa = new float[]{ -accel[1], accel[0], accel[2] };
            ma = new float[]{ -mag[1], mag[0], mag[2] };
        }

        ByteBuffer buf = ByteBuffer.allocate(45).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(GYRO_TAG);
        buf.putLong(timestampMs);
        for (float v : ga) buf.putFloat(v);
        for (float v : aa) buf.putFloat(v);
        for (float v : ma) buf.putFloat(v);

        final byte[] data = buf.array();
        final DatagramSocket fs = s;
        final InetAddress faddr = addr;
        executor.execute(() -> {
            try {
                DatagramPacket packet = new DatagramPacket(data, data.length, faddr, AppConstants.TELEMETRY_PORT);
                fs.send(packet);
            } catch (Exception e) {
                Log.w(TAG, "Telemetry send failed: " + e.getClass().getSimpleName() + ": " + e.getMessage());
                connected = false;
            }
        });
    }

    /** Send fused rotation quaternion (tag 0x12) for driver head tracking. */
    private void sendRotationPacket(long timestampNs) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null) return;

        long timestampMs = timestampNs / 1_000_000;

        ByteBuffer buf = ByteBuffer.allocate(25).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(ROTATION_TAG);
        buf.putLong(timestampMs);
        for (float v : rotationQuat) buf.putFloat(v); // w, x, y, z

        final byte[] data = buf.array();
        final DatagramSocket fs = s;
        final InetAddress faddr = addr;
        executor.execute(() -> {
            try {
                DatagramPacket packet = new DatagramPacket(data, data.length, faddr, AppConstants.TELEMETRY_PORT);
                fs.send(packet);
            } catch (Exception e) {
                Log.w(TAG, "Rotation send failed: " + e.getClass().getSimpleName() + ": " + e.getMessage());
                connected = false;
            }
        });
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    private int getDisplayRotation() {
        try {
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            if (wm != null) {
                Display display = wm.getDefaultDisplay();
                return display.getRotation();
            }
        } catch (Exception ignored) {}
        return Surface.ROTATION_0;
    }
}
