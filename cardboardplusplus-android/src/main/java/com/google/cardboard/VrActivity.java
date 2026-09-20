/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.google.cardboard;
import android.annotation.SuppressLint;
import android.content.Intent;
import android.content.res.AssetManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.opengl.GLSurfaceView;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import com.google.cardboard.camera.CameraController;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.discovery.DiscoveryManager;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.permissions.PermissionManager;
import com.google.cardboard.render.VrRenderer;
import com.google.cardboard.settings.AppSettings;
import com.google.cardboard.settings.SettingsMenuController;
import com.google.cardboard.streaming.CameraStreamer;
import com.google.cardboard.telemetry.TelemetrySender;
import com.google.cardboard.ui.ImmersiveMode;
import com.google.cardboard.video.VideoManager;
@SuppressWarnings("deprecation")
public class VrActivity extends AppCompatActivity implements NativeBridge {
  // Entry point: owns the GL view, native Cardboard SDK handle, and every
  // subsystem (camera, video, discovery, telemetry, camera uplink). Lifecycle
  // is onCreate once, then onResume/onPause per foreground transition; all
  // streaming starts in startSession and stops in onPause.
  static {
    System.loadLibrary("cardboard_jni");
  }
  private static final String TAG = VrActivity.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  private long nativeApp;
  private GLSurfaceView glView;
  private PermissionManager permissionManager;
  private CameraController cameraController;
  private VideoManager videoManager;
  private DiscoveryManager discoveryManager;
  private AppSettings appSettings;
  private CameraStreamer cameraStreamer;
  private TelemetrySender telemetrySender;
  private PowerManager.WakeLock wakeLock;
  private WifiManager.WifiLock wifiLock;
  private SensorManager sensorManager;
  private Sensor proximitySensor;
  // Proximity guard: re-acquires the wake lock when the face covers the
  // sensor, so VR never sleeps mid-session.
  private final SensorEventListener proximityListener =
      new SensorEventListener() {
        @Override
        public void onSensorChanged(SensorEvent event) {
          if (event.sensor.getType() != Sensor.TYPE_PROXIMITY) return;
          acquireWakeLock();
        }
        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {}
      };
  // One-time setup: native SDK handle, prefs, all managers, decoder-cap probe
  // (reported to discovery for the bridge's resolution clamp), GL view with
  // the VrRenderer, touch-to-trigger, sticky immersive mode, max brightness.
  @SuppressLint("ClickableViewAccessibility")
  @Override
  public void onCreate(Bundle savedInstance) {
    super.onCreate(savedInstance);
    nativeApp = nativeOnCreate(getAssets());
    appSettings = new AppSettings(this);
    DebugLog.setGlobalEnabled(appSettings.isDebugLogging());
    DBG.i("VrActivity created, debug=%b", appSettings.isDebugLogging());
    NetworkUtils.init(getApplicationContext());
    cameraStreamer = new CameraStreamer(appSettings);
    telemetrySender = new TelemetrySender(this, appSettings);
    permissionManager = new PermissionManager(this);
    cameraController = new CameraController(this, this);
    videoManager = new VideoManager(this, appSettings);
    videoManager.setTelemetrySender(telemetrySender);
    discoveryManager = new DiscoveryManager(appSettings);
    new Thread(
        () -> {
          int[] decoderCap = videoManager.queryDecoderCap();
          discoveryManager.setDecoderCap(decoderCap[0], decoderCap[1]);
          DBG.i("Decoder cap: %dx%d", decoderCap[0], decoderCap[1]);
        },
        "decoder-cap-query")
        .start();
    videoManager.setReconnectAction(() -> discoveryManager.pokeNow());
    setContentView(R.layout.activity_vr);
    glView = findViewById(R.id.surface_view);
    glView.setEGLContextClientVersion(2);
    glView.setRenderer(
        new VrRenderer(this, cameraController, videoManager, permissionManager));
    glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    glView.setOnTouchListener(
        (v, event) -> {
          if (event.getAction() == MotionEvent.ACTION_DOWN) {
            glView.queueEvent(() -> onTriggerEvent());
            return true;
          }
          return false;
        });
    ImmersiveMode.applySticky(getWindow());
    View decorView = getWindow().getDecorView();
    decorView.setOnSystemUiVisibilityChangeListener(
        (visibility) -> {
          if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) {
            ImmersiveMode.applySticky(getWindow());
          }
        });
    WindowManager.LayoutParams layout = getWindow().getAttributes();
    layout.screenBrightness = 1.f;
    getWindow().setAttributes(layout);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
  }
  // Backgrounding: pauses native SDK, stops discovery/telemetry/camera
  // uplink, releases wake/wifi locks, and pauses camera/video/GL in order.
  @Override
  protected void onPause() {
    super.onPause();
    DBG.i("onPause");
    onNativePause();
    discoveryManager.stopDiscovery();
    cameraStreamer.stop();
    telemetrySender.stop();
    if (sensorManager != null && proximitySensor != null) {
      sensorManager.unregisterListener(proximityListener, proximitySensor);
    }
    if (wakeLock != null && wakeLock.isHeld()) {
      wakeLock.release();
    }
    if (wifiLock != null && wifiLock.isHeld()) {
      wifiLock.release();
    }
    cameraController.onPause();
    videoManager.onPause();
    glView.onPause();
  }
  // Foregrounding: refreshes the debug gate and either waits for permissions
  // or starts the full session (streaming, telemetry, discovery).
  @Override
  protected void onResume() {
    super.onResume();
    DebugLog.setGlobalEnabled(appSettings.isDebugLogging());
    DBG.i("onResume, debug=%b", appSettings.isDebugLogging());
    if (delayResumeUntilPermissionsGranted()) {
      return;
    }
    startSession();
  }
  // Starts everything stream-related: GL resume, native resume, wake/wifi
  // locks, proximity listener, discovery broadcasts, telemetry uplink, video
  // decode (on the GL thread), and camera + its bridge uplink (also GL).
  private void startSession() {
    glView.onResume();
    onNativeResume();
    acquireWakeLock();
    acquireWifiLock();
    if (sensorManager == null) {
      sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
    }
    if (sensorManager != null && proximitySensor == null) {
      proximitySensor = sensorManager.getDefaultSensor(Sensor.TYPE_PROXIMITY);
    }
    if (sensorManager != null && proximitySensor != null) {
      sensorManager.registerListener(
          proximityListener, proximitySensor, SensorManager.SENSOR_DELAY_NORMAL);
    }
    discoveryManager.startDiscovery();
    telemetrySender.start();
    glView.queueEvent(
        () -> {
          if (!videoManager.isStarted()) {
            videoManager.onSurfaceCreated();
            videoManager.start();
          }
        });
    glView.queueEvent(
        () -> {
          if (!cameraController.isTexturePassed()) {
            int textureId = createCameraTexture();
            cameraController.ensureCameraTexture(textureId);
          }
          cameraController.setFrameCallback(cameraStreamer);
          cameraController.openCamera();
          cameraStreamer.start();
        });
  }
  // Keeps the screen bright during VR (non-reference-counted: one acquire,
  // one release in onPause).
  @SuppressLint("Wakelock")
  private void acquireWakeLock() {
    if (wakeLock == null) {
      PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
      if (pm == null) return;
      wakeLock =
          pm.newWakeLock(
              PowerManager.SCREEN_BRIGHT_WAKE_LOCK | PowerManager.ON_AFTER_RELEASE, TAG);
      wakeLock.setReferenceCounted(false);
    }
    if (!wakeLock.isHeld()) {
      wakeLock.acquire();
    }
  }
  // High-performance WiFi lock so the video/telemetry UDP streams never
  // sleep mid-session. Best-effort: logs and continues when denied.
  private void acquireWifiLock() {
    try {
      WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
      if (wm == null) return;
      if (wifiLock == null) {
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, TAG);
        wifiLock.setReferenceCounted(false);
      }
      if (!wifiLock.isHeld()) {
        wifiLock.acquire();
      }
    } catch (Exception e) {
      Log.w(TAG, "WiFi lock acquisition failed: " + e.getMessage());
    }
  }
  // Requests storage (pre-Q) then camera permission in order. True means
  // "hold the session": onResume returns and the grant callback starts it.
  private boolean delayResumeUntilPermissionsGranted() {
    if (VERSION.SDK_INT < VERSION_CODES.Q && !permissionManager.isReadExternalStorageGranted()) {
      permissionManager.requestReadExternalStorage();
      return true;
    }
    if (!permissionManager.isCameraGranted()) {
      permissionManager.requestCamera();
      return true;
    }
    return false;
  }
  // Final teardown: releases the camera and destroys the native SDK handle.
  @Override
  protected void onDestroy() {
    super.onDestroy();
    cameraController.release();
    nativeOnDestroy(nativeApp);
    nativeApp = 0;
  }
  // Re-applies sticky immersive mode whenever the window regains focus
  // (system dialogs clear it).
  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    if (hasFocus) {
      ImmersiveMode.applySticky(getWindow());
    }
  }
  // Layout button: exits the VR sample.
  public void closeSample(View view) {
    if (BuildConfig.DEBUG) Log.d(TAG, "Leaving VR sample");
    finish();
  }
  // Layout button: opens the settings popup (viewer, PC IP, debug).
  public void showSettings(View view) {
    new SettingsMenuController(view, this, appSettings).show();
  }
  // Permission result fan-in: only our two request codes are handled, both
  // delegate to handlePermissionRequestResult.
  @Override
  public void onRequestPermissionsResult(
      int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
    super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    if (requestCode == AppConstants.PERMISSIONS_REQUEST_CODE
        || requestCode == AppConstants.CAMERA_PERMISSIONS_REQUEST_CODE) {
      handlePermissionRequestResult(requestCode);
    }
  }
  // Applies a grant/deny: storage denial exits (with a settings shortcut
  // when permanently denied); camera grant starts the session, denial
  // explains and stays put.
  private void handlePermissionRequestResult(int requestCode) {
    if (requestCode == AppConstants.PERMISSIONS_REQUEST_CODE) {
      if (!permissionManager.isReadExternalStorageGranted()) {
        Toast.makeText(this, R.string.read_storage_permission, Toast.LENGTH_LONG).show();
        if (!permissionManager.shouldShowStorageRationale()) {
          launchPermissionsSettings();
        }
        finish();
      } else {
        onResume();
      }
    } else if (requestCode == AppConstants.CAMERA_PERMISSIONS_REQUEST_CODE) {
      if (permissionManager.isCameraGranted()) {
        Log.i(TAG, "Camera permission granted, starting session");
        startSession();
      } else {
        Toast.makeText(this, "Camera permission is required for passthrough", Toast.LENGTH_LONG)
            .show();
        if (!permissionManager.shouldShowCameraRationale()) {
          launchPermissionsSettings();
        }
      }
    }
  }
  // Opens the app's system settings page for permanently-denied permissions.
  private void launchPermissionsSettings() {
    Intent intent = new Intent();
    intent.setAction(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
    intent.setData(android.net.Uri.fromParts("package", getPackageName(), null));
    startActivity(intent);
  }
  // JNI boundary (implemented in cardboard_jni): lifecycle, per-frame draw,
  // trigger, screen params, viewer switch, camera/video textures, and the UDP
  // video receiver. nativeApp is the opaque handle threading them together.
  private native long nativeOnCreate(AssetManager assetManager);
  private native void nativeOnDestroy(long nativeApp);
  private native void nativeOnSurfaceCreated(long nativeApp);
  private native void nativeOnDrawFrame(long nativeApp);
  private native void nativeOnTriggerEvent(long nativeApp);
  private native void nativeOnPause(long nativeApp);
  private native void nativeOnResume(long nativeApp);
  private native void nativeSetScreenParams(long nativeApp, int width, int height);
  private native void nativeSwitchViewer(long nativeApp);
  private native void nativeOnCameraTextureInitialized(
      long nativeApp, int textureId, int width, int height);
  private native int nativeCreateCameraTexture(long nativeApp);
  private native int nativeCreateVideoTexture(long nativeApp);
  private native void nativeSetVideoDecoder(long nativeApp, Object decoder);
  private native void nativeOnVideoActive(long nativeApp);
  private native void nativeSetVideoVMax(long nativeApp, float vMax);
  private native void nativeResetCameraTexture(long nativeApp);
  private native void nativeStartVideoReceiver(long nativeApp, int port);
  private native void nativeStopVideoReceiver(long nativeApp);
  // NativeBridge impl: one-line forwards into JNI (VrRenderer, VideoManager,
  // and CameraController call these; each just passes nativeApp through).
  @Override
  public void onSurfaceCreated() {
    nativeOnSurfaceCreated(nativeApp);
  }
  @Override
  public void onDrawFrame() {
    nativeOnDrawFrame(nativeApp);
  }
  @Override
  public void onTriggerEvent() {
    nativeOnTriggerEvent(nativeApp);
  }
  @Override
  public void onNativePause() {
    nativeOnPause(nativeApp);
  }
  @Override
  public void onNativeResume() {
    nativeOnResume(nativeApp);
  }
  @Override
  public void setScreenParams(int width, int height) {
    nativeSetScreenParams(nativeApp, width, height);
  }
  @Override
  public void switchViewer() {
    nativeSwitchViewer(nativeApp);
  }
  @Override
  public void onCameraTextureInitialized(int textureId, int width, int height) {
    nativeOnCameraTextureInitialized(nativeApp, textureId, width, height);
  }
  @Override
  public int createCameraTexture() {
    return nativeCreateCameraTexture(nativeApp);
  }
  @Override
  public int createVideoTexture() {
    return nativeCreateVideoTexture(nativeApp);
  }
  @Override
  public void setVideoDecoder(Object decoder) {
    nativeSetVideoDecoder(nativeApp, decoder);
  }
  @Override
  public void onVideoActive() {
    nativeOnVideoActive(nativeApp);
  }
  @Override
  public void setVideoVMax(float vMax) {
    nativeSetVideoVMax(nativeApp, vMax);
  }
  @Override
  public void resetCameraTexture() {
    nativeResetCameraTexture(nativeApp);
  }
  @Override
  public void startVideoReceiver(int port) {
    nativeStartVideoReceiver(nativeApp, port);
  }
  @Override
  public void stopVideoReceiver() {
    nativeStopVideoReceiver(nativeApp);
  }
}
