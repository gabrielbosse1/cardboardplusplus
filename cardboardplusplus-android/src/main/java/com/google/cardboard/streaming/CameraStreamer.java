package com.google.cardboard.streaming;

import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.media.Image;
import android.util.Log;
import com.google.cardboard.camera.CameraController;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

public class CameraStreamer implements CameraController.FrameCallback {
  private static final String TAG = CameraStreamer.class.getSimpleName();
  private static final int PC_PORT = 42072;
  private static final int TARGET_WIDTH = 320;
  private static final int TARGET_HEIGHT = 240;
  private static final int JPEG_QUALITY = 50;
  private static final long FRAME_INTERVAL_MS = 1000 / 15;

  private volatile boolean streaming = false;
  private volatile boolean shouldStream = false;
  private DatagramSocket socket;
  private InetAddress pcAddress;
  private long lastFrameTimeMs;
  private int frameCount;
  private final AppSettings appSettings;
  private Bitmap scaleBuffer;

  public CameraStreamer(AppSettings appSettings) {
    this.appSettings = appSettings;
    lastFrameTimeMs = 0;
    frameCount = 0;
  }

  public synchronized void start() {
    if (shouldStream) return;
    shouldStream = true;
    frameCount = 0;
    Thread t = new Thread(() -> {
      while (shouldStream) {
        try {
          socket = new DatagramSocket();
          pcAddress = NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp());
          streaming = true;
          Log.i(TAG, "Streamer connected to " + pcAddress.getHostAddress() + ":" + PC_PORT);
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
    if (scaleBuffer != null) { scaleBuffer.recycle(); scaleBuffer = null; }
    Log.i(TAG, "Camera streamer stopped, sent " + frameCount + " frames");
  }

  public boolean isStreaming() { return streaming; }

  @Override
  public void onFrame(Image image) {
    if (!streaming || socket == null || pcAddress == null) return;

    long now = System.currentTimeMillis();
    if (now - lastFrameTimeMs < FRAME_INTERVAL_MS) return;
    lastFrameTimeMs = now;

    try {
      byte[] nv21 = imageToNv21(image);
      if (nv21 == null) return;

      int w = image.getWidth();
      int h = image.getHeight();

      YuvImage yuvImage = new YuvImage(nv21, ImageFormat.NV21, w, h, null);
      ByteArrayOutputStream jpegStream = new ByteArrayOutputStream();
      yuvImage.compressToJpeg(new Rect(0, 0, w, h), JPEG_QUALITY, jpegStream);
      byte[] fullJpeg = jpegStream.toByteArray();
      Bitmap fullBmp = android.graphics.BitmapFactory.decodeByteArray(fullJpeg, 0, fullJpeg.length);
      if (fullBmp == null) return;

      if (scaleBuffer == null || scaleBuffer.getWidth() != TARGET_WIDTH || scaleBuffer.getHeight() != TARGET_HEIGHT) {
        if (scaleBuffer != null) scaleBuffer.recycle();
        scaleBuffer = Bitmap.createBitmap(TARGET_WIDTH, TARGET_HEIGHT, Bitmap.Config.ARGB_8888);
      }
      android.graphics.Canvas canvas = new android.graphics.Canvas(scaleBuffer);
      canvas.drawBitmap(fullBmp, null, new Rect(0, 0, TARGET_WIDTH, TARGET_HEIGHT), null);
      fullBmp.recycle();

      jpegStream.reset();
      scaleBuffer.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, jpegStream);
      byte[] jpegData = jpegStream.toByteArray();

      if (jpegData.length > 60000) return;

      DatagramPacket packet = new DatagramPacket(jpegData, jpegData.length, pcAddress, PC_PORT);
      socket.send(packet);
      frameCount++;
      if (frameCount % 60 == 1) {
        Log.i(TAG, "Sent " + frameCount + " frames, last size: " + jpegData.length + " bytes");
      }
    } catch (Exception e) {
      Log.w(TAG, "Frame send failed: " + e.getMessage());
    }
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
    int uvRowStride = uPlane.getRowStride();
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
        int uvOffset = row * uvRowStride + col * uvPixelStride;
        nv21[pos++] = vBuf.get(uvOffset);
        nv21[pos++] = uBuf.get(uvOffset);
      }
    }
    return nv21;
  }
}
