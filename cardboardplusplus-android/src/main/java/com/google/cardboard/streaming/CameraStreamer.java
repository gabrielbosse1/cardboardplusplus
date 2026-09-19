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

public class CameraStreamer implements CameraController.FrameCallback {
  private static final String TAG = CameraStreamer.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  // Wire format: [u16 seq BE][JPEG 256x192 q38]. Downscale YUV first,
  // single JPEG encode — no Bitmap round-trip.
  private static final int TARGET_WIDTH = AppConstants.CAMERA_STREAM_WIDTH;
  private static final int TARGET_HEIGHT = AppConstants.CAMERA_STREAM_HEIGHT;
  private static final int JPEG_QUALITY = AppConstants.CAMERA_JPEG_QUALITY;
  private static final long FRAME_INTERVAL_MS = AppConstants.CAMERA_FRAME_INTERVAL_MS;

  private volatile boolean streaming = false;
  private volatile boolean shouldStream = false;
  private volatile DatagramSocket socket;
  private volatile InetAddress pcAddress;
  private long lastFrameTimeMs;
  private int frameCount;
  private int seq;
  private int droppedFrames;
  private int droppedBusy;
  private final AppSettings appSettings;
  // Dedicated sender thread: the Camera2 callback only hands off the Image
  // and returns, so slow conversion can never stall the capture pipeline.
  private HandlerThread senderThread;
  private Handler senderHandler;
  private final AtomicBoolean senderBusy = new AtomicBoolean(false);

  public CameraStreamer(AppSettings appSettings) {
    this.appSettings = appSettings;
    lastFrameTimeMs = 0;
    frameCount = 0;
  }

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
          while (shouldStream && socket != null && !socket.isClosed()) {
            Thread.sleep(500);
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

  public synchronized void stop() {
    shouldStream = false;
    streaming = false;
    if (socket != null) { socket.close(); socket = null; }
    if (senderThread != null) { senderThread.quitSafely(); senderThread = null; }
    senderHandler = null;
    Log.i(TAG, "Camera streamer stopped, sent " + frameCount + " frames, dropped "
        + droppedFrames + " oversize, " + droppedBusy + " busy");
  }

  public boolean isStreaming() { return streaming; }

  /**
   * Hand the frame to the sender thread and return immediately so the capture
   * pipeline never waits for conversion. Returns true when ownership of
   * {@code image} is taken (the sender thread closes it); false means the
   * caller keeps it and must close it.
   */
  @Override
  public boolean onFrame(Image image) {
    if (!streaming || socket == null || pcAddress == null || senderHandler == null) return false;

    long now = System.currentTimeMillis();
    if (now - lastFrameTimeMs < FRAME_INTERVAL_MS) return false;
    lastFrameTimeMs = now;

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

  /** Convert + send one frame. Runs on the sender thread. */
  private void sendFrame(Image image) {
    DatagramSocket sock = socket;
    InetAddress addr = pcAddress;
    if (sock == null || addr == null) return;
    try {
      int w = image.getWidth();
      int h = image.getHeight();
      if (w <= 0 || h <= 0) return;

      byte[] nv21 = imageToNv21(image);
      if (nv21 == null) return;

      // Downscale NV21 to stream size first, then a single JPEG encode.
      byte[] small = downscaleNv21(nv21, w, h, TARGET_WIDTH, TARGET_HEIGHT);
      YuvImage yuvImage = new YuvImage(small, ImageFormat.NV21, TARGET_WIDTH, TARGET_HEIGHT, null);
      ByteArrayOutputStream jpegStream = new ByteArrayOutputStream();
      yuvImage.compressToJpeg(new Rect(0, 0, TARGET_WIDTH, TARGET_HEIGHT), JPEG_QUALITY, jpegStream);
      byte[] jpegData = jpegStream.toByteArray();

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
      frameCount++;
      if (frameCount % 60 == 1) {
        DBG.i("Sent %d frames, dropped %d oversize %d busy, last size: %d bytes",
            frameCount, droppedFrames, droppedBusy, payload.length);
      }
    } catch (Exception e) {
      Log.w(TAG, "Frame send failed: " + e.getMessage());
    }
  }

  /** Nearest-neighbor downscale of NV21 (Y + interleaved VU planes). */
  static byte[] downscaleNv21(byte[] src, int srcW, int srcH, int dstW, int dstH) {
    byte[] dst = new byte[dstW * dstH * 3 / 2];
    // Y plane.
    for (int y = 0; y < dstH; y++) {
      int srcY = y * srcH / dstH;
      for (int x = 0; x < dstW; x++) {
        dst[y * dstW + x] = src[srcY * srcW + x * srcW / dstW];
      }
    }
    // VU plane (half resolution).
    int srcUvStart = srcW * srcH;
    int dstUvStart = dstW * dstH;
    int srcUvW = srcW / 2;
    int dstUvW = dstW / 2;
    int dstUvH = dstH / 2;
    for (int y = 0; y < dstUvH; y++) {
      int srcY = y * (srcH / 2) / dstUvH;
      for (int x = 0; x < dstUvW; x++) {
        int srcX = x * srcUvW / dstUvW;
        int srcOff = srcUvStart + (srcY * srcUvW + srcX) * 2;
        int dstOff = dstUvStart + (y * dstUvW + x) * 2;
        dst[dstOff] = src[srcOff];
        dst[dstOff + 1] = src[srcOff + 1];
      }
    }
    return dst;
  }

  private static byte[] imageToNv21(Image image) {
    int w = image.getWidth();
    int h = image.getHeight();
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
    byte[] nv21 = new byte[ySize * 3 / 2];

    // Copy Y plane row-by-row.
    int pos = 0;
    for (int row = 0; row < h; row++) {
      yBuf.position(row * yRowStride);
      yBuf.get(nv21, pos, w);
      pos += w;
    }

    // Interleave V and U planes into NV21 (VU order).
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
}
