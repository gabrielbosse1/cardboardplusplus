package com.google.cardboard.video;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Build.VERSION_CODES;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.DebugLog;
import java.nio.ByteBuffer;
import java.util.Arrays;
// Hardware H.264 decoder in video/; VideoManager owns its lifecycle and VrRenderer pumps its output.
public class VideoDecoder {
  private static final String TAG = "VideoDecoder";
  private static final DebugLog DBG = new DebugLog(TAG);
  private final NativeBridge bridge;
  private final int textureId;
  private final int width;
  private final int height;
  private volatile SurfaceTexture surfaceTexture;
  private volatile Surface surface;
  private MediaCodec decoder;
  private volatile long lastFrameAtMs = 0;
  private volatile boolean frameRendered = false;
  private int updateCount = 0;
  private int decodedFrames = 0;
  private volatile int totalDecodedFrames = 0;
  private long lastDecodeLogNs = 0;
  private final MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
  private final Object codecLock = new Object();
  private byte[] sps;
  private byte[] pps;
  private int codedW;
  private int codedH;
  private boolean configured = false;
  // Creates the OES surface and MediaCodec sink; called from VideoManager.onSurfaceCreated on the GL thread.
  public VideoDecoder(NativeBridge bridge, int textureId, int width, int height) {
    this.bridge = bridge;
    this.textureId = textureId;
    this.width = width;
    this.height = height;
    int alignedW = (width + 15) & ~15;
    int alignedH = (height + 15) & ~15;
    surfaceTexture = new SurfaceTexture(textureId);
    surfaceTexture.setDefaultBufferSize(width, height);
    surface = new Surface(surfaceTexture);
    Log.i(TAG, "Created video OES texture=" + textureId + " " + width + "x" + height
        + " (aligned " + alignedW + "x" + alignedH + ")");
  }
  // Drains decoder output into the OES texture; called from VrRenderer via VideoManager.updateTexture on the GL thread.
  public void updateVideoTexture() {
    SurfaceTexture st = surfaceTexture;
    if (st == null) return;
    try {
      boolean doUpdate;
      synchronized (codecLock) {
        doUpdate = drainOutputLocked();
      }
      if (doUpdate && frameRendered) {
        st.updateTexImage();
        frameRendered = false;
      }
    } catch (Exception e) {
      Log.e(TAG, "updateVideoTexture error: " + e.getMessage());
    }
  }
  // Releases output buffers under the codec lock; called from updateVideoTexture on the GL thread.
  private boolean drainOutputLocked() {
    if (configured && decoder != null) {
      int outIdx;
      int drained = 0;
      while ((outIdx = decoder.dequeueOutputBuffer(bufferInfo, 0)) >= 0) {
        decoder.releaseOutputBuffer(outIdx, true);
        frameRendered = true;
        drained++;
        decodedFrames++;
        totalDecodedFrames++;
      }
      updateCount++;
      if (updateCount % 60 == 0) {
        DBG.i("updateVideoTexture #%d drained=%d configured=%b decoderNull=%b", updateCount, drained, configured, decoder == null);
      }
      long nowNs = System.nanoTime();
      if (nowNs - lastDecodeLogNs >= 1_000_000_000L) {
        double fps = decodedFrames * 1e9 / (nowNs - lastDecodeLogNs);
        DBG.i("Decoded video FPS=%.1f (frames=%d)", Math.round(fps * 10) / 10.0, decodedFrames);
        decodedFrames = 0;
        lastDecodeLogNs = nowNs;
      }
      return true;
    } else if (updateCount % 60 == 0) {
      updateCount++;
      DBG.i("updateVideoTexture #%d SKIP configured=%b", updateCount, configured);
    }
    return false;
  }
  // Returns the last decoded-frame timestamp; called from VideoWatchdog and NetStatsReporter polls.
  public long getLastFrameAtMs() {
    return lastFrameAtMs;
  }
  // Returns the lifetime decoded-frame count; called from NetStatsReporter on the reporter thread.
  public int getTotalDecodedFrames() {
    return totalDecodedFrames;
  }
  // Copies a direct buffer into a heap frame; called from the native video receiver on its network thread.
  public void feedDirect(ByteBuffer src, int length, boolean isKey) {
    if (src == null || length <= 0) return;
    byte[] data = new byte[length];
    int oldLimit = src.limit();
    int oldPos = src.position();
    try {
      src.position(0);
      src.limit(length);
      src.get(data, 0, length);
    } catch (Exception e) {
      Log.e(TAG, "feedDirect copy failed: " + e.getMessage());
      return;
    } finally {
      try {
        src.limit(oldLimit);
        src.position(oldPos);
      } catch (Exception ignored) {
      }
    }
    feedFrame(data, isKey);
  }
  // Queues one Annex-B frame, configuring on SPS/PPS first; called from feedDirect on the network thread.
  public void feedFrame(byte[] data, boolean isKey) {
    if (data == null || data.length == 0) return;
    lastFrameAtMs = SystemClock.elapsedRealtime();
    byte[] newSps = isKey ? extractNal(data, 7) : null;
    byte[] newPps = null;
    int[] dims = null;
    if (newSps != null) {
      dims = parseSpsDimensions(newSps);
      if (!Arrays.equals(newSps, sps)) {
        newPps = extractNal(data, 8);
      }
    }
    synchronized (codecLock) {
      boolean changed = false;
      if (newSps != null) {
        if (dims != null && (codedW != dims[0] || codedH != dims[1])) {
          codedW = dims[0];
          codedH = dims[1];
          changed = true;
        }
        if (!Arrays.equals(newSps, sps)) {
          sps = newSps;
          changed = true;
        }
        if (newPps != null && !Arrays.equals(newPps, pps)) {
          pps = newPps;
          changed = true;
        }
        if (changed && configured) {
          Log.i(TAG, "SPS/PPS or resolution changed (->" + codedW + "x" + codedH
              + "); reconfiguring decoder");
          reconfigureDecoderLocked();
        }
      }
      if (!configured) {
        if (sps != null && pps != null) {
          if (!configureLocked()) {
            Log.e(TAG, "MediaCodec configure failed; dropping until next keyframe");
            return;
          }
        } else {
          return;
        }
      }
      try {
        int index = decoder.dequeueInputBuffer(isKey ? 10_000 : 0);
        if (index < 0) {
          if (isKey) Log.w(TAG, "No input buffer for keyframe, dropping frame");
          return;
        }
        ByteBuffer input = decoder.getInputBuffer(index);
        if (input == null) return;
        input.clear();
        input.put(data, 0, data.length);
        int flags = isKey ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
        decoder.queueInputBuffer(index, 0, data.length, 0, flags);
      } catch (Exception e) {
        Log.e(TAG, "feedFrame error: " + e.getMessage());
      }
    }
  }
  // Tears down and rebuilds MediaCodec on SPS change; called from feedFrame under the codec lock.
  private void reconfigureDecoderLocked() {
    try {
      if (decoder != null) {
        decoder.stop();
        decoder.release();
        decoder = null;
      }
    } catch (Exception e) {
      Log.w(TAG, "decoder stop/release during reconfigure failed: " + e.getMessage());
    }
    configured = false;
    configureLocked();
  }
  // Configures MediaCodec from cached SPS/PPS; called from feedFrame and reconfigure under the codec lock.
  private boolean configureLocked() {
    try {
      int baseW = codedW > 0 ? codedW : width;
      int baseH = codedH > 0 ? codedH : height;
      int alignedW = (baseW + 15) & ~15;
      int alignedH = (baseH + 15) & ~15;
      decoder = MediaCodec.createDecoderByType("video/avc");
      MediaFormat format = MediaFormat.createVideoFormat("video/avc", alignedW, alignedH);
      format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
      format.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
      if (Build.VERSION.SDK_INT >= VERSION_CODES.R) {
        format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
      }
      format.setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, 1);
      decoder.configure(format, surface, null, 0);
      if (surfaceTexture != null) {
        surfaceTexture.setDefaultBufferSize(baseW, baseH);
      }
      decoder.start();
      configured = true;
      if (bridge != null) bridge.onVideoActive();
      float vMax = (float) baseH / (float) alignedH;
      if (bridge != null) bridge.setVideoVMax(vMax);
      Log.i(TAG, "MediaCodec configured (SPS=" + sps.length + " PPS=" + pps.length
          + " vMax=" + vMax + ")");
      return true;
    } catch (Exception e) {
      Log.e(TAG, "MediaCodec configure failed: " + e.getMessage());
      decoder = null;
      return false;
    }
  }
  // Releases codec, surface, and texture; called from VideoManager.onPause and onSurfaceCreated.
  public void release() {
    synchronized (codecLock) {
      releaseLocked();
    }
    Log.i(TAG, "VideoDecoder released");
  }
  // Releases codec and GL objects without logging; called from release under the codec lock.
  private void releaseLocked() {
    try {
      if (decoder != null) {
        decoder.stop();
        decoder.release();
        decoder = null;
      }
    } catch (Exception e) {
      Log.w(TAG, "decoder release failed: " + e.getMessage());
    }
    try {
      if (surface != null) {
        surface.release();
        surface = null;
      }
    } catch (Exception e) {
    }
    try {
      if (surfaceTexture != null) {
        surfaceTexture.release();
        surfaceTexture = null;
      }
    } catch (Exception e) {
    }
    configured = false;
    sps = null;
    pps = null;
  }
  private static byte[] extractNal(byte[] data, int nalType) {
    return H264NalParser.extractNal(data, nalType);
  }
  private static int[] parseSpsDimensions(byte[] sps) {
    return H264NalParser.parseSpsDimensions(sps);
  }
}
