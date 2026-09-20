package com.google.cardboard.video;
import android.util.Log;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.settings.AppSettings;
import com.google.cardboard.telemetry.TelemetrySender;
// Playback coordinator in video/; VrActivity and VrRenderer drive surface, start, texture, and pause.
public class VideoManager {
  private static final String TAG = VideoManager.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  private final NativeBridge bridge;
  private final AppSettings appSettings;
  private VideoDecoder decoder;
  private boolean surfaceCreated = false;
  private Runnable reconnectAction;
  private VideoWatchdog watchdog;
  private NetStatsReporter netStats;
  private TelemetrySender telemetrySender;
  // Stores the native bridge and settings; called from VrActivity.onCreate on the UI thread.
  public VideoManager(NativeBridge bridge, AppSettings appSettings) {
    this.bridge = bridge;
    this.appSettings = appSettings;
  }
  // Registers the telemetry path for net-stats; called from VrActivity.onCreate on the UI thread.
  public void setTelemetrySender(TelemetrySender telemetrySender) {
    this.telemetrySender = telemetrySender;
  }
  // Returns the hardware decode ceiling; called from VrActivity's decoder-cap thread.
  public int[] queryDecoderCap() {
    return DecoderCapabilityReporter.queryDecoderCapability();
  }
  // Registers the stall-recovery hook; called from VrActivity.onCreate with DiscoveryManager.pokeNow.
  public void setReconnectAction(Runnable reconnectAction) {
    this.reconnectAction = reconnectAction;
  }
  // Creates the decoder on a fresh video texture; called from VrActivity start and VrRenderer setup on the GL thread.
  public void onSurfaceCreated() {
    if (decoder != null) {
      decoder.release();
      decoder = null;
    }
    int texId = bridge.createVideoTexture();
    decoder =
        new VideoDecoder(
            bridge, texId, AppConstants.DEFAULT_VIDEO_WIDTH, AppConstants.DEFAULT_VIDEO_HEIGHT);
    bridge.setVideoDecoder(decoder);
    surfaceCreated = true;
    Log.i(TAG, "Video decoder created (tex=" + texId + ")");
  }
  // Starts native reception plus watchdog and net-stats; called from VrActivity start and VrRenderer setup on the GL thread.
  public void start() {
    if (!surfaceCreated) {
      Log.w(TAG, "start() called before surface created; ignoring");
      return;
    }
    bridge.startVideoReceiver(AppConstants.VIDEO_PORT);
    Log.i(TAG, "Video receiver started on port " + AppConstants.VIDEO_PORT);
    startWatchdog();
    startNetStats();
  }
  // Lazily starts the stall watchdog; called from start on the GL thread.
  private void startWatchdog() {
    if (watchdog == null) {
      watchdog = new VideoWatchdog(TAG, reconnectAction, () -> decoder);
    }
    watchdog.start();
  }
  // Stops the stall watchdog; called from onPause on the UI thread.
  private void stopWatchdog() {
    if (watchdog != null) {
      watchdog.stop();
    }
  }
  // Lazily starts net-stats reporting; called from start on the GL thread.
  private void startNetStats() {
    if (netStats == null) {
      netStats = new NetStatsReporter(appSettings, () -> decoder, telemetrySender);
    }
    netStats.start();
  }
  // Stops net-stats reporting; called from onPause on the UI thread.
  private void stopNetStats() {
    if (netStats != null) {
      netStats.stop();
    }
  }
  // Drains decoder output into the GL texture; called from VrRenderer.onDrawFrame on the GL thread.
  public void updateTexture() {
    if (decoder != null) {
      decoder.updateVideoTexture();
    }
  }
  // Releases the decoder and stops native reception; called from VrActivity.onPause on the UI thread.
  public void onPause() {
    stopWatchdog();
    stopNetStats();
    if (decoder != null) {
      decoder.release();
      decoder = null;
    }
    bridge.stopVideoReceiver();
    surfaceCreated = false;
  }
  // Reports whether the decoder surface exists; called from VrActivity startSession on the GL thread.
  public boolean isStarted() {
    return surfaceCreated;
  }
}
