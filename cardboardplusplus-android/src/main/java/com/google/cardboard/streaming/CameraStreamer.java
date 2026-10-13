package com.google.cardboard.streaming;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.media.Image;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import com.google.cardboard.camera.CameraController;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.util.concurrent.atomic.AtomicBoolean;
// Phone camera uplink: converts Camera2 frames to small JPEGs and sends one
// UDP datagram each ([u16 seq BE][JPEG]) to the bridge on port 42072 for the
// MediaPipe sidecar. Implements CameraController.FrameCallback; VrActivity
// owns start/stop. Drops (never queues) when the sender is busy — latest
// frame wins.
public class CameraStreamer implements CameraController.FrameCallback {
  private static final String TAG = CameraStreamer.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  // JPEG quality from AppConstants; frames are encoded at native capture
  // size (bridge decodes any size, MediaPipe resizes internally).
  private static final int JPEG_QUALITY = AppConstants.CAMERA_JPEG_QUALITY;
  // Set while the socket loop runs; false during reconnect gaps (frames are
  // refused then). Distinct from shouldStream, the stop flag.
  private volatile boolean streaming = false;
  private volatile boolean shouldStream = false;
  private volatile DatagramSocket socket;
  private volatile InetAddress pcAddress;
  private int frameCount;
  private int seq;
  private int droppedFrames;
  private int droppedBusy;
  private final AppSettings appSettings;
  private HandlerThread senderThread;
  private Handler senderHandler;
  // Guards the single in-flight encode: onFrame drops when the sender thread
  // is still busy, so a slow JPEG never builds a backlog.
  private final AtomicBoolean senderBusy = new AtomicBoolean(false);
  // Reused NV21 conversion buffer (no per-frame allocation).
  private byte[] nv21Scratch;
  public CameraStreamer(AppSettings appSettings) {
    this.appSettings = appSettings;
    frameCount = 0;
  }
  // Opens the sender thread plus a daemon socket loop (bind, resolve PC,
  // re-resolve every 500ms/5s like TelemetrySender). Idempotent.
  public synchronized void start() {
    if (shouldStream) return;
    shouldStream = true;
    frameCount = 0;
    droppedBusy = 0;
    senderThread = new HandlerThread("CameraSender");
    senderThread.start();
    senderHandler = new Handler(senderThread.getLooper());
    Thread t = new Thread(() -> {
      while (shouldStream) {
        try {
          socket = new DatagramSocket();
          pcAddress = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
          streaming = true;
          Log.i(TAG, "Streamer connected to " + pcAddress.getHostAddress() + ":" + AppConstants.CAMERA_PORT);
          String lastPcIp = appSettings.getPcIp();
          if (lastPcIp == null) lastPcIp = "";
          long lastResolveMs = System.currentTimeMillis();
          while (shouldStream && socket != null && !socket.isClosed()) {
            Thread.sleep(500);
            try {
              String cur = appSettings.getPcIp();
              if (cur == null) cur = "";
              long now = System.currentTimeMillis();
              if (!cur.equals(lastPcIp) || now - lastResolveMs > 5000) {
                InetAddress fresh = NetworkUtils.getPcOrBroadcastAddress(cur);
                lastPcIp = cur;
                lastResolveMs = now;
                if (!fresh.equals(pcAddress)) {
                  pcAddress = fresh;
                  Log.i(TAG, "Streamer retargeted to " + fresh.getHostAddress()
                      + ":" + AppConstants.CAMERA_PORT);
                }
              }
            } catch (Exception e) {
              Log.w(TAG, "Streamer retarget failed: " + e.getClass().getSimpleName());
            }
          }
        } catch (Exception e) {
          Log.w(TAG, "Connection failed: " + e.getMessage() + ", retrying in 2s...");
        }
        streaming = false;
        if (!shouldStream) break;
        try { Thread.sleep(2000); } catch (InterruptedException ie) { break; }
      }
    });
    t.setDaemon(true);
    t.start();
  }
  // Stops socket + thread and logs lifetime counters (sent / oversize /
  // busy drops) for the logcat record.
  public synchronized void stop() {
    shouldStream = false;
    streaming = false;
    if (socket != null) { socket.close(); socket = null; }
    if (senderThread != null) { senderThread.quitSafely(); senderThread = null; }
    senderHandler = null;
    Log.i(TAG, "Camera streamer stopped, sent " + frameCount + " frames, dropped "
        + droppedFrames + " oversize, " + droppedBusy + " busy");
  }
  // FrameCallback entry (camera thread): uncapped, drops when the sender
  // is busy, else hands the Image to the sender thread (which closes it).
  // True means accepted for send.
  public boolean isStreaming() { return streaming; }
  @Override
  public boolean onFrame(Image image) {
    if (!streaming || socket == null || pcAddress == null || senderHandler == null) return false;
    if (!senderBusy.compareAndSet(false, true)) {
      droppedBusy++;
      return false;
    }
    senderHandler.post(() -> {
      try {
        sendFrame(image);
      } finally {
        try {
          image.close();
        } catch (Exception ignored) {
        }
        senderBusy.set(false);
      }
    });
    return true;
  }
  // Converts one frame to NV21, JPEGs it at native capture size, and sends
  // [seq][jpeg] as a single datagram. Oversize JPEGs (past the 60KB
  // datagram cap) are counted and dropped — UDP cannot fragment them.
  private void sendFrame(Image image) {
    DatagramSocket sock = socket;
    InetAddress addr = pcAddress;
    if (sock == null || addr == null) return;
    try {
      int w = image.getWidth();
      int h = image.getHeight();
      if (w <= 0 || h <= 0) return;
      long t0 = System.nanoTime();
      byte[] nv21 = imageToNv21(image, w, h);
      if (nv21 == null) return;
      long t1 = System.nanoTime();
      YuvImage yuvImage = new YuvImage(nv21, ImageFormat.NV21, w, h, null);
      ByteArrayOutputStream jpegStream = new ByteArrayOutputStream();
      yuvImage.compressToJpeg(new Rect(0, 0, w, h), JPEG_QUALITY, jpegStream);
      byte[] jpegData = jpegStream.toByteArray();
      long t2 = System.nanoTime();
      if (jpegData.length + AppConstants.CAMERA_SEQ_HEADER_LEN > AppConstants.CAMERA_MAX_DATAGRAM) {
        droppedFrames++;
        return;
      }
      byte[] payload = new byte[AppConstants.CAMERA_SEQ_HEADER_LEN + jpegData.length];
      payload[0] = (byte) ((seq >> 8) & 0xFF);
      payload[1] = (byte) (seq & 0xFF);
      System.arraycopy(jpegData, 0, payload, AppConstants.CAMERA_SEQ_HEADER_LEN, jpegData.length);
      seq++;
      DatagramPacket packet = new DatagramPacket(payload, payload.length, addr, AppConstants.CAMERA_PORT);
      sock.send(packet);
      long t3 = System.nanoTime();
      frameCount++;
      if (frameCount % 60 == 1) {
        DBG.i("Sent %d frames %dx%d, dropped %d oversize %d busy, last size: %d bytes, nv21=%dms jpeg=%dms send=%dms",
            frameCount, w, h, droppedFrames, droppedBusy, payload.length,
            (t1 - t0) / 1000000, (t2 - t1) / 1000000, (t3 - t2) / 1000000);
      }
    } catch (Exception e) {
      Log.w(TAG, "Frame send failed: " + e.getMessage());
    }
  }
  // Repacks a YUV_420_888 Image (arbitrary row/pixel strides) into packed
  // NV21 (V/U interleaved) in the reused scratch buffer.
  private byte[] imageToNv21(Image image, int w, int h) {
    Image.Plane yPlane = image.getPlanes()[0];
    Image.Plane uPlane = image.getPlanes()[1];
    Image.Plane vPlane = image.getPlanes()[2];
    ByteBuffer yBuf = yPlane.getBuffer();
    ByteBuffer uBuf = uPlane.getBuffer();
    ByteBuffer vBuf = vPlane.getBuffer();
    int yRowStride = yPlane.getRowStride();
    int uRowStride = uPlane.getRowStride();
    int vRowStride = vPlane.getRowStride();
    int uvPixelStride = uPlane.getPixelStride();
    int ySize = w * h;
    byte[] nv21 = ensureNv21(ySize * 3 / 2);
    int pos = 0;
    for (int row = 0; row < h; row++) {
      yBuf.position(row * yRowStride);
      yBuf.get(nv21, pos, w);
      pos += w;
    }
    int uvHeight = h / 2;
    int uvWidth = w / 2;
    for (int row = 0; row < uvHeight; row++) {
      for (int col = 0; col < uvWidth; col++) {
        int vOffset = row * vRowStride + col * uvPixelStride;
        int uOffset = row * uRowStride + col * uvPixelStride;
        nv21[pos++] = vBuf.get(vOffset);
        nv21[pos++] = uBuf.get(uOffset);
      }
    }
    return nv21;
  }
  // Growable scratch buffers: reused across frames to keep the camera path
  // allocation-free after warmup.
  private byte[] ensureNv21(int need) {
    if (nv21Scratch == null || nv21Scratch.length < need) {
      nv21Scratch = new byte[need];
    }
    return nv21Scratch;
  }
}
