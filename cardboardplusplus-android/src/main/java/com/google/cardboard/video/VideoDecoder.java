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

/**
 * Hardware H.264 decoder backed by Android {@link MediaCodec}, outputting directly into a
 * {@link SurfaceTexture} (GL_TEXTURE_EXTERNAL_OES). The native UDP receiver forwards each H.264
 * access unit here via {@link #feedFrame(byte[], boolean)}; the GL thread calls
 * {@link #updateVideoTexture()} (which invokes SurfaceTexture.updateTexImage) just before drawing,
 * so the decoded frame is sampled with zero CPU copy.
 */
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
  // Guards decoder/config/SPS state shared by the decode-forwarding thread
  // (feed) and the GL thread (drain). NAL scanning happens outside the lock.
  private final Object codecLock = new Object();

  // Codec-specific data (SPS/PPS) collected from the first keyframe(s).
  private byte[] sps;
  private byte[] pps;
  private int codedW;
  private int codedH;
  private boolean configured = false;

  public VideoDecoder(NativeBridge bridge, int textureId, int width, int height) {
    this.bridge = bridge;
    this.textureId = textureId;
    this.width = width;
    this.height = height;

    // The hardware decoder (c2.qti) requires 16x16-macroblock-aligned dimensions.
    // The actual stream is 2880x1620, but the coded (SPS) height is 1632; align up.
    int alignedW = (width + 15) & ~15;
    int alignedH = (height + 15) & ~15;
    surfaceTexture = new SurfaceTexture(textureId);
    // Use actual video dimensions for the SurfaceTexture, not the macroblock-aligned
    // dimensions. The codec outputs at alignedH but the bottom rows are uninitialized
    // padding — showing them causes a green line artifact on both eyes.
    surfaceTexture.setDefaultBufferSize(width, height);
    surface = new Surface(surfaceTexture);
    Log.i(TAG, "Created video OES texture=" + textureId + " " + width + "x" + height
        + " (aligned " + alignedW + "x" + alignedH + ")");
  }

  // ---------------------------------------------------------------------------
  // GL presentation: drain decoder output, then sample the latest frame once per draw
  // ---------------------------------------------------------------------------

  /** Called from the GL thread before drawing, to present the latest decoded frame. */
  public void updateVideoTexture() {
    SurfaceTexture st = surfaceTexture;
    if (st == null) return;
    try {
      // The OES texture (textureId) is created by native and already lives in the
      // GL context, so no attachToGLContext is needed. Just drain decoder output
      // into the Surface and sample the latest frame.
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

  /** Drain decoder output into the Surface. Caller must hold {@link #codecLock}. */
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

  /** Time (elapsedRealtime ms) of the last received frame; 0 if none yet. */
  public long getLastFrameAtMs() {
    return lastFrameAtMs;
  }

  /** Monotonic count of decoded output frames (for the net-stats reporter). */
  public int getTotalDecodedFrames() {
    return totalDecodedFrames;
  }

  // ---------------------------------------------------------------------------
  // Ingress: feed H.264 access units; lazy-configure once SPS+PPS are known
  // ---------------------------------------------------------------------------

  /**
   * Feed one frame from the native direct buffer. Copies out of the shared
   * buffer immediately (the native side reuses it for the next frame) and
   * delegates to {@link #feedFrame(byte[], boolean)}.
   */
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

  /**
   * Feed one H.264 access unit (annexb, start-code prefixed). The decoder is configured lazily once
   * SPS and PPS have been observed in the stream. SPS/PPS only arrive in
   * keyframes, so non-keyframes skip the rescan entirely (isKey comes from the
   * receiver's single scan, passed through native).
   */
  public void feedFrame(byte[] data, boolean isKey) {
    if (data == null || data.length == 0) return;
    lastFrameAtMs = SystemClock.elapsedRealtime();

    // Refresh SPS/PPS whenever present so a resolution change from the PC
    // (e.g. the encoder re-inits after applying the phone's hardware cap) is
    // detected and the decoder reconfigured instead of silently breaking.
    // Scanning happens outside the codec lock; only state updates take it.
    byte[] newSps = isKey ? extractNal(data, 7) : null;
    byte[] newPps = null;
    int[] dims = null;
    if (newSps != null) {
      dims = parseSpsDimensions(newSps);
      // Fast path: identical SPS bytes mean nothing changed — skip the PPS
      // scan too (SPS+PPS always travel together in x264 streams).
      if (!Arrays.equals(newSps, sps)) {
        newPps = extractNal(data, 8);
      }
    }

    synchronized (codecLock) {
      boolean changed = false;
      if (newSps != null) {
        // Dims-first: a cheap int compare detects resolution changes before
        // the byte compare decides whether the codec-specific data changed.
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
          // Still waiting for codec-specific data; drop until we have SPS+PPS.
          return;
        }
      }

      try {
        // Under pressure drop P-frames immediately (0-timeout) but never drop
        // keyframes: without SPS/IDR the decoder can't configure and every
        // following P-frame would corrupt, so keyframes keep a short wait.
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

  // ---------------------------------------------------------------------------
  // Codec configuration (initial + reconfiguration on SPS/PPS/resolution change)
  // ---------------------------------------------------------------------------

  /** Stop and re-create the decoder with the current SPS/PPS (resolution change).
   *  Caller must hold {@link #codecLock}. */
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
    // configureLocked() will use the updated sps/pps/codedW/codedH.
    configureLocked();
  }

  private boolean configureLocked() {
    try {
      // Use the actual coded resolution from SPS so the MediaFormat matches the
      // stream (and stays within the hardware decoder's width cap).
      int baseW = codedW > 0 ? codedW : width;
      int baseH = codedH > 0 ? codedH : height;
      int alignedW = (baseW + 15) & ~15;
      int alignedH = (baseH + 15) & ~15;

      decoder = MediaCodec.createDecoderByType("video/avc");

      MediaFormat format = MediaFormat.createVideoFormat("video/avc", alignedW, alignedH);
      format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
      format.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
      // Prefer low-latency decode on Android 11+; allow frame drop so the decoder
      // never blocks the (already-decoded) SurfaceTexture when we render slower.
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
      // Tell the OES shader to skip macroblock padding rows at the bottom of
      // the decoded frame. The codec outputs at alignedH but only the top
      // baseH rows contain valid content; the rest are green garbage.
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

  public void release() {
    synchronized (codecLock) {
      releaseLocked();
    }
    Log.i(TAG, "VideoDecoder released");
  }

  /** Caller must hold {@link #codecLock}. */
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
      // ignore
    }
    try {
      if (surfaceTexture != null) {
        surfaceTexture.release();
        surfaceTexture = null;
      }
    } catch (Exception e) {
      // ignore
    }
    configured = false;
    sps = null;
    pps = null;
  }

  // --- annexb NAL extraction ---------------------------------------------------
  //
  // The raw bitstream parsing (start-code scanning, NAL extraction, SPS dimension
  // decoding) lives in H264NalParser so this class only owns the MediaCodec state.

  /** Returns the first NAL unit of type {@code nalType} (e.g. 7 = SPS) or null. */
  private static byte[] extractNal(byte[] data, int nalType) {
    return H264NalParser.extractNal(data, nalType);
  }

  /** Parse the coded (SPS) width/height from an annexb SPS NAL (with start code). */
  private static int[] parseSpsDimensions(byte[] sps) {
    return H264NalParser.parseSpsDimensions(sps);
  }
}
