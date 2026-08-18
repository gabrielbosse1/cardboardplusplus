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

  private final NativeBridge bridge;
  private final int textureId;
  private final int width;
  private final int height;

  private SurfaceTexture surfaceTexture;
  private Surface surface;
  private MediaCodec decoder;

  private volatile long lastFrameAtMs = 0;
  private boolean frameRendered = false;
  private int updateCount = 0;
  private int decodedFrames = 0;
  private long lastDecodeLogNs = 0;
  private final MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();

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
    surfaceTexture.setDefaultBufferSize(alignedW, alignedH);
    surface = new Surface(surfaceTexture);
    Log.i(TAG, "Created video OES texture=" + textureId + " " + width + "x" + height
        + " (aligned " + alignedW + "x" + alignedH + ")");
  }

  // ---------------------------------------------------------------------------
  // GL presentation: drain decoder output, then sample the latest frame once per draw
  // ---------------------------------------------------------------------------

  /** Called from the GL thread before drawing, to present the latest decoded frame. */
  public void updateVideoTexture() {
    if (surfaceTexture == null) return;
    try {
      // The OES texture (textureId) is created by native and already lives in the
      // GL context, so no attachToGLContext is needed. Just drain decoder output
      // into the Surface and sample the latest frame.
      if (configured && decoder != null) {
        int outIdx;
        int drained = 0;
        while ((outIdx = decoder.dequeueOutputBuffer(bufferInfo, 0)) >= 0) {
          decoder.releaseOutputBuffer(outIdx, true);
          frameRendered = true;
          drained++;
          decodedFrames++;
        }
        updateCount++;
        if (updateCount % 60 == 0) {
          Log.i(TAG, "updateVideoTexture #" + updateCount + " drained=" + drained
              + " configured=" + configured + " decoderNull=" + (decoder == null));
        }
        long nowNs = System.nanoTime();
        if (nowNs - lastDecodeLogNs >= 1_000_000_000L) {
          double fps = decodedFrames * 1e9 / (nowNs - lastDecodeLogNs);
          Log.i(TAG, "Decoded video FPS=" + Math.round(fps * 10) / 10.0
              + " (frames=" + decodedFrames + ")");
          decodedFrames = 0;
          lastDecodeLogNs = nowNs;
        }
      } else if (updateCount % 60 == 0) {
        updateCount++;
        Log.i(TAG, "updateVideoTexture #" + updateCount + " SKIP configured=" + configured);
      }

      if (frameRendered) {
        surfaceTexture.updateTexImage();
        frameRendered = false;
      }
    } catch (Exception e) {
      Log.e(TAG, "updateVideoTexture error: " + e.getMessage());
    }
  }

  /** Time (elapsedRealtime ms) of the last received frame; 0 if none yet. */
  public long getLastFrameAtMs() {
    return lastFrameAtMs;
  }

  // ---------------------------------------------------------------------------
  // Ingress: feed H.264 access units; lazy-configure once SPS+PPS are known
  // ---------------------------------------------------------------------------

  /**
   * Feed one H.264 access unit (annexb, start-code prefixed). The decoder is configured lazily once
   * SPS and PPS have been observed in the stream.
   */
  public synchronized void feedFrame(byte[] data, boolean isKey) {
    if (data == null || data.length == 0) return;
    lastFrameAtMs = SystemClock.elapsedRealtime();

    // Refresh SPS/PPS whenever present so a resolution change from the PC
    // (e.g. the encoder re-inits after applying the phone's hardware cap) is
    // detected and the decoder reconfigured instead of silently breaking.
    byte[] newSps = extractNal(data, 7);
    byte[] newPps = extractNal(data, 8);
    if (newSps != null) {
      int[] dims = parseSpsDimensions(newSps);
      boolean changed = false;
      if (!Arrays.equals(newSps, sps)) { sps = newSps; changed = true; }
      if (newPps != null && !Arrays.equals(newPps, pps)) { pps = newPps; changed = true; }
      if (dims != null && (codedW != dims[0] || codedH != dims[1])) {
        codedW = dims[0];
        codedH = dims[1];
        changed = true;
      }
      if (changed && configured) {
        Log.i(TAG, "SPS/PPS or resolution changed (->" + codedW + "x" + codedH
            + "); reconfiguring decoder");
        reconfigureDecoder();
      }
    }

    if (!configured) {
      if (sps != null && pps != null) {
        if (!configure()) {
          Log.e(TAG, "MediaCodec configure failed; dropping until next keyframe");
          return;
        }
      } else {
        // Still waiting for codec-specific data; drop until we have SPS+PPS.
        return;
      }
    }

    try {
      int index = decoder.dequeueInputBuffer(10_000);
      if (index < 0) {
        Log.w(TAG, "No input buffer available, dropping frame");
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

  // ---------------------------------------------------------------------------
  // Codec configuration (initial + reconfiguration on SPS/PPS/resolution change)
  // ---------------------------------------------------------------------------

  /** Stop and re-create the decoder with the current SPS/PPS (resolution change). */
  private void reconfigureDecoder() {
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
    // configure() will use the updated sps/pps/codedW/codedH.
    configure();
  }

  private boolean configure() {
    try {
      // Use the actual coded resolution from SPS so the MediaFormat matches the
      // stream (and stays within the hardware decoder's width cap).
      int baseW = codedW > 0 ? codedW : width;
      int baseH = codedH > 0 ? codedH : height;
      int alignedW = (baseW + 15) & ~15;
      int alignedH = (baseH + 15) & ~15;
      MediaFormat format = MediaFormat.createVideoFormat("video/avc", alignedW, alignedH);
      format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
      format.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
      // Prefer low-latency decode on Android 11+; allow frame drop so the decoder
      // never blocks the (already-decoded) SurfaceTexture when we render slower.
      if (Build.VERSION.SDK_INT >= VERSION_CODES.R) {
        format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
      }
      format.setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, 1);
      // Allow the codec to adapt to larger resolutions without a hard failure.
      format.setInteger(MediaFormat.KEY_MAX_WIDTH, 4096);
      format.setInteger(MediaFormat.KEY_MAX_HEIGHT, 4096);

      decoder = MediaCodec.createDecoderByType("video/avc");
      try {
        android.media.MediaCodecInfo.VideoCapabilities vc =
            decoder.getCodecInfo().getCapabilitiesForType("video/avc").getVideoCapabilities();
        Log.i(TAG, "Codec supported widths=" + vc.getSupportedWidths()
            + " heights=" + vc.getSupportedHeights()
            + " upper=" + vc.getSupportedWidths().getUpper()
            + "x" + vc.getSupportedHeights().getUpper());
      } catch (Exception e) {
        Log.w(TAG, "could not query video capabilities: " + e.getMessage());
      }
      decoder.configure(format, surface, null, 0);
      if (surfaceTexture != null) {
        surfaceTexture.setDefaultBufferSize(alignedW, alignedH);
      }
      decoder.start();
      configured = true;
      if (bridge != null) bridge.onVideoActive();
      Log.i(TAG, "MediaCodec configured (SPS=" + sps.length + " PPS=" + pps.length + ")");
      return true;
    } catch (Exception e) {
      Log.e(TAG, "MediaCodec configure failed: " + e.getMessage());
      decoder = null;
      return false;
    }
  }

  public synchronized void release() {
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
    Log.i(TAG, "VideoDecoder released");
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
