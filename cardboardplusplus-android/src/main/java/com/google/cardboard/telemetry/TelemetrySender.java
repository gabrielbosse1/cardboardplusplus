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
// Phone motion uplink: fuses accel/gyro/magnetometer + the rotation-vector
// sensor and streams tag-0x10 gyro frames (10Hz raw) and tag-0x12 fused
// orientation quats to the bridge on UDP 42071. The 0x12 quats are the live
// head-tracking path (bridge forwards them to the driver on UDP 42074).
// Runs its sensors on a dedicated HandlerThread; VrActivity owns start/stop.
public class TelemetrySender implements SensorEventListener {
    private static final String TAG = TelemetrySender.class.getSimpleName();
    private static final DebugLog DBG = new DebugLog(TAG);
    private static final byte GYRO_TAG = AppConstants.TELEMETRY_TAG_GYRO;
    private static final byte ROTATION_TAG = AppConstants.TELEMETRY_TAG_ROTATION;
    private final Context context;
    private final SensorManager sensorManager;
    private final AppSettings appSettings;
    private volatile DatagramSocket socket;
    private volatile InetAddress bridgeAddress;
    private volatile boolean connected;
    private final float[] accel = new float[3];
    private final float[] gyro = new float[3];
    private final float[] mag = new float[3];
    // Latest fused orientation quat [w,x,y,z] in SteamVR space, written by
    // handleRotationVector on the sensor thread and read by sendRotationPacket.
    private final float[] rotationQuat = new float[4];
    // Raw-frame throttle: at most one 0x10 packet per 100ms (accel/gyro/mag
    // events fire far faster; the bridge only needs the latest).
    private long lastRawNs = 0;
    private static final long MIN_RAW_INTERVAL_NS = 100_000_000L;
    // Pre-sized LE buffers for the 45-byte gyro and 25-byte rotation frames;
    // reused every send to avoid allocation on the sensor thread.
    private final ByteBuffer rawBuf = ByteBuffer.allocate(45).order(ByteOrder.LITTLE_ENDIAN);
    private final ByteBuffer rotBuf = ByteBuffer.allocate(25).order(ByteOrder.LITTLE_ENDIAN);
    // Scratch packet reused by both send paths (single sensor thread only).
    private final DatagramPacket sendPacket = new DatagramPacket(new byte[0], 0);
    private HandlerThread sensorThread;
    private Handler sensorHandler;
    private final float[] tmpA = new float[4];
    private final float[] tmpB = new float[4];
    private int cachedRotation = Surface.ROTATION_0;
    private long lastRotCheckMs = 0;
    private static final long ROTATION_CACHE_MS = 1000;
    private volatile boolean running;
    public TelemetrySender(Context context, AppSettings appSettings) {
        this.context = context;
        this.sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.appSettings = appSettings;
    }
    // Starts sensor listeners at GAME rate on a dedicated thread plus the
    // connect loop. Prefers the drift-free game rotation vector, falls back
    // to the magnetometer-based one. Called from VrActivity.onResume.
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
        for (Sensor s : sensorManager.getSensorList(Sensor.TYPE_ROTATION_VECTOR)) {
            DBG.d("ROT_SENSOR avail: type=11 vendor=%s name=%s handle=%d", s.getVendor(), s.getName(), s.getName().hashCode());
        }
        for (Sensor s : sensorManager.getSensorList(Sensor.TYPE_GAME_ROTATION_VECTOR)) {
            DBG.d("GAME_SENSOR avail: type=15 vendor=%s name=%s handle=%d", s.getVendor(), s.getName(), s.getName().hashCode());
        }
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
        if (rotVecSensor != null) sensorManager.registerListener(this, rotVecSensor, delay, sensorHandler);
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
    // Unregisters everything, closes the socket, and stops the sensor thread.
    // Called from VrActivity.onPause; safe to call when never started.
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
    // Daemon loop: binds a socket, resolves the PC (settings IP or broadcast),
    // announces the phone hello, then every 500ms re-resolves (roaming PC /
    // DHCP change) and re-hellos on retarget. Sleeps 2s between reconnects.
    private void connectLoop() {
        while (running) {
            try {
                DatagramSocket s = new DatagramSocket();
                InetAddress addr = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
                socket = s;
                bridgeAddress = addr;
                connected = true;
                Log.i(TAG, "Telemetry connected to " + addr.getHostAddress() + ":" + AppConstants.TELEMETRY_PORT);
                try {
                    byte[] hello = phoneHelloBytes();
                    s.send(new DatagramPacket(hello, hello.length, addr, AppConstants.TELEMETRY_PORT));
                    DBG.d("Sent phone hello to %s:%d", addr.getHostAddress(), AppConstants.TELEMETRY_PORT);
        } catch (Exception e) {
          Log.w(TAG, "phone hello send failed", e);
        }
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
    // Builds the "CARDBOARD_PHONE_HELLO vN" connect announcement with the
    // app version suffix the bridge logs. Static for the contract test.
    static byte[] phoneHelloBytes() {
        return AppConstants.phoneHello(BuildConfig.VERSION_NAME).getBytes();
    }
    // Shared UDP send for other phone components (camera streamer reuses the
    // connected socket). False when down; a send failure marks disconnected
    // so connectLoop re-establishes.
    public boolean sendDatagram(byte[] data) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null || !connected) return false;
        try {
            s.send(new DatagramPacket(data, data.length, addr, AppConstants.TELEMETRY_PORT));
            return true;
        } catch (Exception e) {
            Log.w(TAG, "Shared telemetry send failed: " + e.getClass().getSimpleName());
            connected = false;
            return false;
        }
    }
    // Sensor callback (runs on the TelemetrySensors thread): caches the three
    // raw axes and throttles raw frames to 10Hz; every rotation-vector event
    // is transformed and sent immediately as the head pose.
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
    // Fixed remap chain (Android sensor frame -> SteamVR frame): world tilt,
    // per-display-rotation inverse, then a 180-degree roll. Q_DEV_INV is
    // indexed by Surface rotation (0/90/180/270).
    private static final float S = 0.70710678f;
    private static final float[] Q_WORLD = {S, -S, 0f, 0f};
    private static final float[] Q_ROLL180 = {0f, 0f, 0f, 1f};
    private static final float[][] Q_DEV_INV = {
        {1f, 0f, 0f, 0f},
        {S, 0f, 0f, S},
        {0f, 0f, 0f, -1f},
        {S, 0f, 0f, -S},
    };
    // Quaternion multiply a*b into out ([w,x,y,z] order). No allocation: the
    // remap chain in handleRotationVector runs per sensor event.
    private static void qmulInto(float[] a, float[] b, float[] out) {
        out[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
        out[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
        out[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
        out[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
    }
    // Remaps one rotation-vector sample into SteamVR space and emits the
    // 0x12 quat packet immediately (unthrottled: this is the head pose).
    // Derives w when the sensor omits it (3-component events).
    private void handleRotationVector(SensorEvent event) {
        float x = event.values[0], y = event.values[1], z = event.values[2];
        float w = event.values.length > 3 ? event.values[3]
                : (float) Math.sqrt(Math.max(0f, 1f - x * x - y * y - z * z));
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
    // Emits a raw gyro frame at most every 100ms, driven by whichever raw
    // sensor fired (accel/gyro/mag share one throttle clock).
    private void maybeSendRaw(long timestampNs) {
        if (timestampNs - lastRawNs < MIN_RAW_INTERVAL_NS) return;
        lastRawNs = timestampNs;
        sendRawPacket(timestampNs);
    }
    // Builds and sends the 45-byte tag-0x10 frame (timestamp + gyro/accel/mag
    // triples), remapping x/y for 90/270-degree displays. Reuses rawBuf and
    // the shared sendPacket; send failure marks the link down.
    private void sendRawPacket(long timestampNs) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null) return;
        long timestampMs = timestampNs / 1_000_000;
        rawBuf.clear();
        rawBuf.put(GYRO_TAG);
        rawBuf.putLong(timestampMs);
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
    // Appends one xyz triple to a telemetry buffer.
    private static void putVec3(ByteBuffer buf, float x, float y, float z) {
        buf.putFloat(x);
        buf.putFloat(y);
        buf.putFloat(z);
    }
    // Builds and sends the 25-byte tag-0x12 frame (timestamp + [w,x,y,z]).
    // Same reuse/failure contract as sendRawPacket.
    private void sendRotationPacket(long timestampNs) {
        DatagramSocket s = socket;
        InetAddress addr = bridgeAddress;
        if (s == null || s.isClosed() || addr == null) return;
        long timestampMs = timestampNs / 1_000_000;
        rotBuf.clear();
        rotBuf.put(ROTATION_TAG);
        rotBuf.putLong(timestampMs);
        for (float v : rotationQuat) rotBuf.putFloat(v);
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
    // Unused (interface requirement); accuracy changes need no handling.
    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    // Cached display rotation (refreshed at most 1Hz): remaps both raw axes
    // and quats when the phone is held landscape.
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
