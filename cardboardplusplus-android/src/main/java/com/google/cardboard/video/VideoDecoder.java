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
//
// Stream lifecycle on the phone (the native VideoReceiver drives it):
//   Idle      no codec picture on screen; the renderer draws black.
//   Waiting   frames arrive; nothing but a keyframe is decoded.
//   Streaming the first decoded picture is on the texture; onVideoActive().
// Every end of a stream (driver goodbye, 2 s without video, decode failure) goes
// through endStreamLocked: the codec is stopped and released, cached SPS/PPS are
// dropped and onVideoInactive() puts the renderer back to black. The next
// keyframe configures a fresh codec, so a new stream needs no restart.
public class VideoDecoder {
  private static final String TAG = "VideoDecoder";
  private static final DebugLog DBG = new DebugLog(TAG);
  // Upper bound accepted when parsing a keyframe's SPS; the driver clamps its
  // encoder to 320x180-7680x7680, so anything outside that is malformed.
  private static final int MAX_STREAM_DIM = 7680;
  // How long a frame may wait for a codec input buffer. Any frame, not only
  // keyframes: dropping a P-frame here and feeding the next one is what used to
  // smear the picture until the next IDR.
  private static final long INPUT_TIMEOUT_MS = 20;
  // Poll step while waiting. The codec lock is released between polls so the GL
  // thread's output drain (which frees input buffers) is never blocked.
  private static final long INPUT_POLL_MS = 2;
  // endStream reasons; 1 and 2 mirror VideoReceiver::EndReason in native code.
  public static final int END_GOODBYE = 1;
  public static final int END_TIMEOUT = 2;
  public static final int END_DECODE_ERROR = 3;
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
  // Guarded by codecLock. needKeyframe: the codec has no valid reference, so
  // P-frames are dropped until a keyframe. visible: a picture of the current
  // stream is on the texture and the renderer shows it. generation: bumped on
  // every teardown so a GL-thread update from a finished stream cannot turn the
  // picture back on.
  private boolean needKeyframe = true;
  private boolean visible = false;
  private int generation = 0;
  private long lastPtsUs = 0;
  private int gatedDrops = 0;
  private volatile Runnable keyframeRequester;
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
  // Registers the KEYFRAME_REQ sender; called from VideoManager when the decoder is created.
  public void setKeyframeRequester(Runnable requester) {
    this.keyframeRequester = requester;
  }
  // Asks the driver for an IDR; called outside codecLock (it sends a datagram).
  private void requestKeyframe(String why) {
    Runnable r = keyframeRequester;
    DBG.w("Requesting keyframe: %s", why);
    if (r != null) {
      r.run();
    }
  }
  // Drains decoder output into the OES texture; called from VrRenderer via VideoManager.updateTexture on the GL thread.
  public void updateVideoTexture() {
    SurfaceTexture st = surfaceTexture;
    if (st == null) return;
    try {
      boolean doUpdate;
      boolean showPending;
      int gen;
      synchronized (codecLock) {
        doUpdate = drainOutputLocked();
        showPending = doUpdate && frameRendered && !visible;
        gen = generation;
      }
      if (doUpdate && frameRendered) {
        st.updateTexImage();
        frameRendered = false;
      }
      if (showPending) {
        // The texture now holds this stream's first picture; only now may the
        // renderer stop drawing black, so a stale frame is never shown.
        synchronized (codecLock) {
          if (gen == generation && configured && !visible) {
            visible = true;
            if (bridge != null) bridge.onVideoActive();
            Log.i(TAG, "First picture of the stream on screen");
          }
        }
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
  // Queues one complete Annex-B access unit, configuring on SPS/PPS first;
  // called from feedDirect on the native decode thread. The native receiver
  // already drops incomplete frames and gates on keyframes after loss; this
  // gate covers what can go wrong on this side (no input buffer, codec reset).
  public void feedFrame(byte[] data, boolean isKey) {
    if (data == null || data.length == 0) return;
    lastFrameAtMs = SystemClock.elapsedRealtime();
    byte[] newSps = isKey ? extractNal(data, 7) : null;
    byte[] newPps = null;
    int[] dims = null;
    if (newSps != null) {
      dims = parseSpsDimensions(newSps);
      // Only trust an SPS that parses into a plausible frame size, otherwise it
      // would be cached and reconfigure the codec into a broken state.
      if (dims == null || dims[0] <= 0 || dims[0] > MAX_STREAM_DIM || dims[1] <= 0
          || dims[1] > MAX_STREAM_DIM) {
        newSps = null;
        dims = null;
      }
      if (newSps != null) {
        newPps = extractNal(data, 8);
      }
    }
    String requestWhy = null;
    int gen;
    synchronized (codecLock) {
      gen = generation;
      if (needKeyframe && !isKey) {
        gatedDrops++;
        if (gatedDrops % 30 == 1) {
          DBG.w("Dropping P-frames until a keyframe (dropped=%d)", gatedDrops);
        }
        return;
      }
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
        if (sps == null || pps == null) {
          return;
        }
        if (!configureLocked()) {
          Log.e(TAG, "MediaCodec configure failed; back to idle until the next keyframe");
          endStreamLocked(END_DECODE_ERROR);
          requestWhy = "configure failed";
        }
      }
    }
    if (requestWhy == null) {
      requestWhy = queueFrame(data, isKey, gen);
    }
    if (requestWhy != null) {
      requestKeyframe(requestWhy);
    }
  }
  // Waits up to INPUT_TIMEOUT_MS for a codec input buffer, polling without
  // holding the lock in between. Gives up if the stream was torn down meanwhile.
  // Returns a keyframe-request reason, or null when the frame was queued (or
  // the stream ended and the frame no longer matters).
  private String queueFrame(byte[] data, boolean isKey, int gen) {
    long deadline = SystemClock.elapsedRealtime() + INPUT_TIMEOUT_MS;
    while (true) {
      synchronized (codecLock) {
        if (gen != generation || !configured || decoder == null) {
          return null;
        }
        boolean lastTry = SystemClock.elapsedRealtime() >= deadline;
        String result = queueLocked(data, isKey, lastTry);
        if (result != RETRY) {
          return result;
        }
      }
      try {
        Thread.sleep(INPUT_POLL_MS);
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
        return null;
      }
    }
  }
  private static final String RETRY = "retry";
  // Hands one frame to MediaCodec. Returns null when queued, RETRY when no input
  // buffer is free yet (and this is not the last try), otherwise a
  // keyframe-request reason. Caller holds codecLock.
  private String queueLocked(byte[] data, boolean isKey, boolean lastTry) {
    try {
      int index = decoder.dequeueInputBuffer(0);
      if (index < 0 && !lastTry) {
        return RETRY;
      }
      if (index < 0) {
        // Skipping a frame breaks every later P-frame's reference: stop and
        // wait for a keyframe instead of decoding garbage.
        needKeyframe = true;
        Log.w(TAG, "No codec input buffer within " + INPUT_TIMEOUT_MS
            + "ms; frame dropped, waiting for a keyframe");
        return "codec input full";
      }
      ByteBuffer input = decoder.getInputBuffer(index);
      if (input == null || input.capacity() < data.length) {
        decoder.queueInputBuffer(index, 0, 0, nextPtsUs(), 0);
        needKeyframe = true;
        Log.w(TAG, "Codec input buffer too small for " + data.length + " bytes; frame dropped");
        return "codec input too small";
      }
      input.clear();
      input.put(data, 0, data.length);
      int flags = isKey ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
      decoder.queueInputBuffer(index, 0, data.length, nextPtsUs(), flags);
      if (isKey) {
        if (needKeyframe) {
          DBG.i("Keyframe queued; decoding resumes (gated drops=%d)", gatedDrops);
        }
        needKeyframe = false;
        gatedDrops = 0;
      }
      return null;
    } catch (Exception e) {
      // CodecException / IllegalStateException: the codec is unusable. That is
      // a terminal transition: tear down to idle and pull a fresh IDR.
      Log.e(TAG, "Decode failure: " + e.getMessage() + "; back to idle");
      endStreamLocked(END_DECODE_ERROR);
      return "decode failure";
    }
  }
  // Strictly increasing presentation time; MediaCodec may reorder or drop
  // output when every input carries the same timestamp.
  private long nextPtsUs() {
    long now = SystemClock.elapsedRealtimeNanos() / 1000;
    lastPtsUs = now > lastPtsUs ? now : lastPtsUs + 1;
    return lastPtsUs;
  }
  // Ends the current stream; called from the native decode thread on driver
  // goodbye or receive timeout. Flushes and releases the codec and returns the
  // renderer to the idle (black) screen.
  public void endStream(int reason) {
    synchronized (codecLock) {
      endStreamLocked(reason);
    }
  }
  // Terminal transition shared by every stream end. Caller holds codecLock.
  private void endStreamLocked(int reason) {
    try {
      if (decoder != null) {
        try {
          decoder.flush();
        } catch (Exception ignored) {
        }
        decoder.stop();
        decoder.release();
      }
    } catch (Exception e) {
      Log.w(TAG, "decoder release during stream end failed: " + e.getMessage());
    }
    decoder = null;
    configured = false;
    sps = null;
    pps = null;
    codedW = 0;
    codedH = 0;
    needKeyframe = true;
    frameRendered = false;
    gatedDrops = 0;
    generation++;
    visible = false;
    if (bridge != null) bridge.onVideoInactive();
    Log.i(TAG, "Stream ended (" + reasonName(reason) + "): decoder released, idle screen");
  }
  private static String reasonName(int reason) {
    switch (reason) {
      case END_GOODBYE:
        return "driver goodbye";
      case END_TIMEOUT:
        return "timeout";
      case END_DECODE_ERROR:
        return "decode failure";
      default:
        return "reason " + reason;
    }
  }
  // Rebuilds the codec from the cached SPS/PPS; called from VideoWatchdog when
  // frames keep arriving but nothing decodes. Leaves sps/pps cached so the next
  // keyframe still refreshes them if the driver re-emits different ones.
  public void reconfigure() {
    synchronized (codecLock) {
      if (!configured || decoder == null) {
        return;
      }
      DBG.w("Frames arriving but none decoded; reconfiguring decoder");
      reconfigureDecoderLocked();
      // A fresh codec has no reference pictures: feeding it the P-frames that
      // follow would decode garbage, so wait for the IDR the watchdog requests.
      needKeyframe = true;
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
    decoder = null;
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
      // onVideoActive() waits for the first decoded picture (updateVideoTexture),
      // so a texture still holding the previous stream is never shown.
      float vMax = (float) baseH / (float) alignedH;
      if (bridge != null) bridge.setVideoVMax(vMax);
      Log.i(TAG, "MediaCodec configured (SPS=" + sps.length + " PPS=" + pps.length
          + " vMax=" + vMax + ")");
      return true;
    } catch (Exception e) {
      Log.e(TAG, "MediaCodec configure failed: " + e.getMessage());
      if (decoder != null) {
        try {
          decoder.release();
        } catch (Exception ignored) {
        }
      }
      decoder = null;
      configured = false;
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
    decoder = null;
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
    needKeyframe = true;
    generation++;
    visible = false;
  }
  private static byte[] extractNal(byte[] data, int nalType) {
    return H264NalParser.extractNal(data, nalType);
  }
  private static int[] parseSpsDimensions(byte[] sps) {
    return H264NalParser.parseSpsDimensions(sps);
  }
}
