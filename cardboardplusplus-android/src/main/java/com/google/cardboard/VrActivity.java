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
import androidx.annotation.NonNull;
import android.opengl.GLSurfaceView;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import com.google.cardboard.camera.CameraController;
import com.google.cardboard.codec.CodecSelector;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.discovery.DiscoveryManager;
import com.google.cardboard.permissions.PermissionManager;
import com.google.cardboard.render.VrRenderer;
import com.google.cardboard.settings.AppSettings;
import com.google.cardboard.settings.SettingsMenuController;
import com.google.cardboard.streaming.CameraStreamer;
import com.google.cardboard.video.VideoManager;

/**
 * Entry point / orchestrator for the Cardboard++ VR app.
 *
 * <p>This Activity owns the native app instance and the JNI surface (native methods are hard-bound
 * to this class name in the C++ layer), and wires together the extracted subsystems: camera, video
 * receiver, discovery, permissions, settings and rendering. Subsystem behaviour lives in their own
 * packages and is reached through {@link NativeBridge}.
 */
// TODO(b/184737638): Remove decorator once the AndroidX migration is completed.
@SuppressWarnings("deprecation")
public class VrActivity extends AppCompatActivity implements NativeBridge {
  static {
    System.loadLibrary("cardboard_jni");
  }

  private static final String TAG = VrActivity.class.getSimpleName();

  // Opaque native pointer to the native CardboardApp instance.
  // This object is owned by the VrActivity instance and passed to the native methods.
  private long nativeApp;

  private GLSurfaceView glView;

  private PermissionManager permissionManager;
  private CameraController cameraController;
  private VideoManager videoManager;
  private DiscoveryManager discoveryManager;
  private AppSettings appSettings;
  private CodecSelector codecSelector;
  private CameraStreamer cameraStreamer;

  @SuppressLint("ClickableViewAccessibility")
  @Override
  public void onCreate(Bundle savedInstance) {
    super.onCreate(savedInstance);

    nativeApp = nativeOnCreate(getAssets());

    appSettings = new AppSettings(this);
    codecSelector = new CodecSelector(appSettings);
    cameraStreamer = new CameraStreamer(appSettings);
    permissionManager = new PermissionManager(this);
    cameraController = new CameraController(this, this);
    videoManager = new VideoManager(this);
    discoveryManager = new DiscoveryManager();

    setContentView(R.layout.activity_vr);
    glView = findViewById(R.id.surface_view);
    glView.setEGLContextClientVersion(2);
    glView.setRenderer(
        new VrRenderer(this, cameraController, videoManager, permissionManager));
    glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    glView.setOnTouchListener(
        (v, event) -> {
          if (event.getAction() == MotionEvent.ACTION_DOWN) {
            // Signal a trigger event.
            glView.queueEvent(() -> onTriggerEvent());
            return true;
          }
          return false;
        });

    // TODO(b/139010241): Avoid that action and status bar are displayed when pressing settings
    // button.
    setImmersiveSticky();
    View decorView = getWindow().getDecorView();
    decorView.setOnSystemUiVisibilityChangeListener(
        (visibility) -> {
          if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) {
            setImmersiveSticky();
          }
        });

    // Forces screen to max brightness.
    WindowManager.LayoutParams layout = getWindow().getAttributes();
    layout.screenBrightness = 1.f;
    getWindow().setAttributes(layout);

    // Prevents screen from dimming/locking.
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
  }

  @Override
  protected void onPause() {
    super.onPause();

    // 1. Tell native to stop head tracking FIRST
    onNativePause();

    // 2. Stop discovery
    discoveryManager.stopDiscovery();

    // 3. Stop camera hardware and release texture so it gets recreated fresh on resume
    cameraController.onPause();

    // 3b. Tear down the video decoder + receiver.
    videoManager.onPause();

    // 4. Stop GL thread LAST
    glView.onPause();
  }

  @Override
  protected void onResume() {
    super.onResume();

    // On Android P and below, checks for activity to READ_EXTERNAL_STORAGE. When it is not granted,
    // the application will request them. For Android Q and above, READ_EXTERNAL_STORAGE is optional
    // and scoped storage will be used instead. If it is provided (but not checked) and there are
    // device parameters saved in external storage those will be migrated to scoped storage.
    if (VERSION.SDK_INT < VERSION_CODES.Q && !permissionManager.isReadExternalStorageGranted()) {
      permissionManager.requestReadExternalStorage();
      return;
    }

    // Check camera permission
    if (!permissionManager.isCameraGranted()) {
      permissionManager.requestCamera();
      return;
    }

    glView.onResume();
    onNativeResume();

    discoveryManager.startDiscovery();

    // Queue camera setup on GL thread (guards prevent duplicates)
    glView.queueEvent(
        () -> {
          if (!cameraController.isTexturePassed()) {
            int textureId = createCameraTexture();
            cameraController.ensureCameraTexture(textureId);
          }
          cameraController.openCamera();
        });
  }

  @Override
  protected void onDestroy() {
    super.onDestroy();
    cameraController.release();
    nativeOnDestroy(nativeApp);
    nativeApp = 0;
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    if (hasFocus) {
      setImmersiveSticky();
    }
  }

  /** Callback for when close button is pressed. */
  public void closeSample(View view) {
    Log.d(TAG, "Leaving VR sample");
    finish();
  }

  /** Callback for when settings_menu button is pressed. */
  public void showSettings(View view) {
    new SettingsMenuController(view, this).show();
  }

  /**
   * Callback for the result from requesting permissions.
   *
   * <p>When READ_EXTERNAL_STORAGE permission is not granted, the settings view will be launched
   * with a toast explaining why it is required.
   */
  @Override
  public void onRequestPermissionsResult(
      int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
    super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    if (requestCode == AppConstants.PERMISSIONS_REQUEST_CODE) {
      if (!permissionManager.isReadExternalStorageGranted()) {
        Toast.makeText(this, R.string.read_storage_permission, Toast.LENGTH_LONG).show();
        if (!permissionManager.shouldShowStorageRationale()) {
          launchPermissionsSettings();
        }
        finish();
      }
    } else if (requestCode == AppConstants.CAMERA_PERMISSIONS_REQUEST_CODE) {
      if (permissionManager.isCameraGranted()) {
        // Camera will be initialized in onSurfaceCreated when GL context is ready
        Log.i(TAG, "Camera permission granted, will initialize on GL thread");
      } else {
        Toast.makeText(this, "Camera permission is required for passthrough", Toast.LENGTH_LONG)
            .show();
      }
    }
  }

  private void launchPermissionsSettings() {
    Intent intent = new Intent();
    intent.setAction(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
    intent.setData(android.net.Uri.fromParts("package", getPackageName(), null));
    startActivity(intent);
  }

  private void setImmersiveSticky() {
    getWindow()
        .getDecorView()
        .setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
  }

  // ---------------------------------------------------------------------------
  // Native glue (JNI). Method names are bound to Java_com_google_cardboard_VrActivity_*
  // in the C++ layer, so these declarations must remain in this class.
  // ---------------------------------------------------------------------------

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

  private native void nativeResetCameraTexture(long nativeApp);

  private native void nativeSetEyeTexture(long nativeApp, int eye, int textureId);

  private native void nativeStartVideoReceiver(long nativeApp, int port);

  private native void nativeStopVideoReceiver(long nativeApp);

  private native void nativeUpdateVideoTexture(long nativeApp);

  private native boolean nativeHasVideoFrame(long nativeApp);

  // ---------------------------------------------------------------------------
  // NativeBridge implementation
  // ---------------------------------------------------------------------------

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
  public void resetCameraTexture() {
    nativeResetCameraTexture(nativeApp);
  }

  @Override
  public void setEyeTexture(int eye, int textureId) {
    nativeSetEyeTexture(nativeApp, eye, textureId);
  }

  @Override
  public void startVideoReceiver(int port) {
    nativeStartVideoReceiver(nativeApp, port);
  }

  @Override
  public void stopVideoReceiver() {
    nativeStopVideoReceiver(nativeApp);
  }

  @Override
  public void updateVideoTexture() {
    nativeUpdateVideoTexture(nativeApp);
  }

  @Override
  public boolean hasVideoFrame() {
    return nativeHasVideoFrame(nativeApp);
  }
}
