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

  // Set by the SurfaceTexture frame-available callback; consumed by the GL
  // thread before sampling the texture. Avoids calling updateTexImage() before
  // the codec has produced a frame (which races with the codec's surface connect).
  private volatile boolean frameAvailable = false;
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
    surfaceTexture.setOnFrameAvailableListener(st -> frameAvailable = true);
    surface = new Surface(surfaceTexture);
    Log.i(TAG, "Created video OES texture=" + textureId + " " + width + "x" + height
        + " (aligned " + alignedW + "x" + alignedH + ")");
  }

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

  /** Parse the coded (SPS) width/height from an annexb SPS NAL (with start code). */
  private static int[] parseSpsDimensions(byte[] sps) {
    try {
      int n = sps.length;
      // Skip start code (3 or 4 bytes).
      int off = (n > 4 && sps[2] == 1) ? 3 : (n > 5 && sps[3] == 1 ? 4 : 0);
      // Remove emulation prevention bytes (00 00 03 -> 00 00) for bit parsing.
      byte[] rbsp = new byte[n];
      int rlen = 0;
      for (int i = off; i < n; i++) {
        if (i + 2 < n && sps[i] == 0 && sps[i + 1] == 0 && sps[i + 2] == 3) {
          rbsp[rlen++] = sps[i];
          rbsp[rlen++] = sps[i + 1];
          i += 2; // skip the 03
        } else {
          rbsp[rlen++] = sps[i];
        }
      }
      BitReader br = new BitReader(rbsp, rlen);
      br.skip(8); // NAL header (profile/type already known)
      int profileIdc = br.read(8);
      br.skip(8); // constraint flags + reserved
      br.skip(8); // level_idc
      br.readUe(); // seq_parameter_set_id
      if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122
          || profileIdc == 244 || profileIdc == 44 || profileIdc == 83
          || profileIdc == 86 || profileIdc == 118 || profileIdc == 128
          || profileIdc == 138 || profileIdc == 139 || profileIdc == 134
          || profileIdc == 135) {
        br.readUe(); // chroma_format_idc
        br.readUe(); // bit_depth_luma_minus8
        br.readUe(); // bit_depth_chroma_minus8
        br.skip(1); // qpprime
        int seqScaling = br.read(1);
        if (seqScaling == 1) {
          // scaling lists (skip 8x for 4x4 and 8x for 8x8, simplified)
          for (int i = 0; i < 8; i++) {
            int size = (i < 6) ? 16 : 64;
            int present = br.read(1);
            if (present == 1) {
              for (int j = 0; j < size; j++) br.readSe();
            }
          }
        }
      }
      br.readUe(); // log2_max_frame_num_minus4
      int pocType = br.readUe();
      if (pocType == 0) {
        br.readUe(); // log2_max_pic_order_cnt_lsb_minus4
      } else if (pocType == 1) {
        br.skip(1);
        br.readSe();
        br.readSe();
        int cycles = br.readUe();
        for (int i = 0; i < cycles; i++) br.readSe();
      }
      br.readUe(); // max_num_ref_frames
      br.skip(1); // gaps_in_frame_num_value_allowed_flag
      int wMbs = br.readUe() + 1;
      int hMbs = br.readUe() + 1;
      int frameMbsOnly = br.read(1);
      if (frameMbsOnly == 0) br.skip(1);
      br.skip(1); // direct_8x8_inference_flag
      int codedW = wMbs * 16;
      int codedH = hMbs * 16;
      if (frameMbsOnly == 0) codedH *= 2;
      return new int[] { codedW, codedH };
    } catch (Exception e) {
      return null;
    }
  }

  private static final class BitReader {
    private final byte[] b;
    private final int len;
    private int pos; // bit position
    BitReader(byte[] b, int len) { this.b = b; this.len = len; }
    void skip(int n) { pos += n; }
    int read(int n) {
      int v = 0;
      for (int i = 0; i < n; i++) {
        int byteIdx = pos >> 3;
        if (byteIdx >= len) { pos++; continue; }
        int bit = (b[byteIdx] >> (7 - (pos & 7))) & 1;
        v = (v << 1) | bit;
        pos++;
      }
      return v;
    }
    int readUe() {
      int zeros = 0;
      while (read(1) == 0 && zeros < 32) zeros++;
      if (zeros == 0) return 0;
      return (1 << zeros) - 1 + read(zeros);
    }
    int readSe() {
      int k = readUe();
      if (k % 2 == 0) return -(k / 2);
      return (k + 1) / 2;
    }
  }



  /** Returns the first NAL unit of the given type (with its start code) or null. */
  private static byte[] extractNal(byte[] data, int nalType) {    int n = data.length;
    int i = 0;
    while (i + 4 < n) {
      int sc = startCodeLen(data, i);
      if (sc == 0) {
        i++;
        continue;
      }
      int type = data[i + sc] & 0x1F;
      if (type == nalType) {
        int next = findNextStart(data, i + sc);
        int end = (next < 0) ? n : next;
        return Arrays.copyOfRange(data, i, end);
      }
      i += sc;
    }
    return null;
  }

  private static int startCodeLen(byte[] d, int i) {
    if (i + 3 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
      return 4;
    }
    if (i + 2 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      return 3;
    }
    return 0;
  }

  private static int findNextStart(byte[] d, int from) {
    for (int i = from; i + 3 < d.length; i++) {
      if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) return i;
      if (i + 2 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    }
    return -1;
  }
}
