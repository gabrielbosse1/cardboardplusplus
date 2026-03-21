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

import android.Manifest;
import android.annotation.SuppressLint;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.Settings;
import android.util.Size;
import android.view.Surface;
import androidx.appcompat.app.AppCompatActivity;
import android.util.Log;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.PopupMenu;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;
import java.nio.ByteBuffer;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;

/**
 * A Google Cardboard VR NDK sample application.
 *
 * <p>This is the main Activity for the sample application. It initializes a GLSurfaceView to allow
 * rendering.
 */
// TODO(b/184737638): Remove decorator once the AndroidX migration is completed.
@SuppressWarnings("deprecation")
public class VrActivity extends AppCompatActivity implements PopupMenu.OnMenuItemClickListener {
  static {
    System.loadLibrary("cardboard_jni");
  }

  private static final String TAG = VrActivity.class.getSimpleName();

  // Permission request codes
  private static final int PERMISSIONS_REQUEST_CODE = 2;
  private static final int CAMERA_PERMISSIONS_REQUEST_CODE = 3;

  // Opaque native pointer to the native CardboardApp instance.
  // This object is owned by the VrActivity instance and passed to the native methods.
  private long nativeApp;

  private GLSurfaceView glView;

  // Camera2 related members
  private CameraManager cameraManager;
  private CameraDevice cameraDevice;
  private CameraCaptureSession captureSession;
  private SurfaceTexture cameraSurfaceTexture;
  private Surface cameraSurface;
  private HandlerThread cameraThread;
  private Handler cameraHandler;
  private boolean cameraInitialized = false;
  private boolean cameraTexturePassed = false;
  private int cameraWidth = 640;
  private int cameraHeight = 480;

  @SuppressLint("ClickableViewAccessibility")
  @Override
  public void onCreate(Bundle savedInstance) {
    super.onCreate(savedInstance);

    nativeApp = nativeOnCreate(getAssets());

    setContentView(R.layout.activity_vr);
    glView = findViewById(R.id.surface_view);
    glView.setEGLContextClientVersion(2);
    Renderer renderer = new Renderer();
    glView.setRenderer(renderer);
    glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    glView.setOnTouchListener(
        (v, event) -> {
          if (event.getAction() == MotionEvent.ACTION_DOWN) {
            // Signal a trigger event.
            glView.queueEvent(
                () -> {
                  nativeOnTriggerEvent(nativeApp);
                });
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

  private boolean checkCameraPermission() {
    return ActivityCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
        == PackageManager.PERMISSION_GRANTED;
  }

  private void requestCameraPermission() {
    ActivityCompat.requestPermissions(this,
        new String[] {Manifest.permission.CAMERA}, CAMERA_PERMISSIONS_REQUEST_CODE);
  }

  private void createCaptureSession() {
    try {
      final CaptureRequest.Builder captureRequestBuilder =
          cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
      captureRequestBuilder.addTarget(cameraSurface);

      cameraDevice.createCaptureSession(
          new java.util.ArrayList<Surface>() {{ add(cameraSurface); }},
          new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession session) {
              captureSession = session;
              try {
                captureRequestBuilder.set(CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                captureRequestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_ON);
                captureSession.setRepeatingRequest(captureRequestBuilder.build(), null, cameraHandler);
              } catch (CameraAccessException e) {
                Log.e(TAG, "Failed to start preview: " + e.getMessage());
              }
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession session) {
              Log.e(TAG, "Camera configuration failed");
            }
          },
          cameraHandler);
    } catch (CameraAccessException e) {
      Log.e(TAG, "Failed to create capture session: " + e.getMessage());
    }
  }

  @Override
  protected void onPause() {
    super.onPause();
    nativeOnPause(nativeApp);
    glView.onPause();
  }

  @Override
  protected void onResume() {
    super.onResume();

    // On Android P and below, checks for activity to READ_EXTERNAL_STORAGE. When it is not granted,
    // the application will request them. For Android Q and above, READ_EXTERNAL_STORAGE is optional
    // and scoped storage will be used instead. If it is provided (but not checked) and there are
    // device parameters saved in external storage those will be migrated to scoped storage.
    if (VERSION.SDK_INT < VERSION_CODES.Q && !isReadExternalStorageEnabled()) {
      requestPermissions();
      return;
    }

    // Check camera permission
    if (!checkCameraPermission()) {
      requestCameraPermission();
      return;
    }

    glView.onResume();
    nativeOnResume(nativeApp);
  }

  @SuppressLint("MissingPermission")
  private void initCameraOnGlThreadWithTexture(int textureId) {
    if (cameraInitialized || cameraSurfaceTexture != null) {
      return;
    }

    // Initialize camera manager
    cameraManager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);

    try {
      String[] cameraIds = cameraManager.getCameraIdList();
      String backCameraId = null;
      for (String id : cameraIds) {
        CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(id);
        Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
        if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
          backCameraId = id;
          break;
        }
      }

      if (backCameraId == null) {
        Log.e(TAG, "No back camera found");
        return;
      }

      CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(backCameraId);
      android.hardware.camera2.params.StreamConfigurationMap map =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      Size[] outputSizes = map.getOutputSizes(SurfaceTexture.class);

      if (outputSizes != null && outputSizes.length > 0) {
        for (Size size : outputSizes) {
          if (size.getWidth() >= 640 && size.getHeight() >= 480) {
            cameraWidth = size.getWidth();
            cameraHeight = size.getHeight();
            break;
          }
        }
      }

      // Create SurfaceTexture with pre-created texture ID
      cameraSurfaceTexture = new SurfaceTexture(textureId);
      cameraSurfaceTexture.setDefaultBufferSize(cameraWidth, cameraHeight);
      cameraSurface = new Surface(cameraSurfaceTexture);

      // Setup camera thread
      cameraThread = new HandlerThread("CameraThread");
      cameraThread.start();
      cameraHandler = new Handler(cameraThread.getLooper());

      // Open camera
      cameraManager.openCamera(backCameraId, new CameraDevice.StateCallback() {
        @Override
        public void onOpened(CameraDevice camera) {
          cameraDevice = camera;
          createCaptureSession();
        }

        @Override
        public void onDisconnected(CameraDevice camera) {
          camera.close();
          cameraDevice = null;
        }

        @Override
        public void onError(CameraDevice camera, int error) {
          camera.close();
          cameraDevice = null;
        }
      }, cameraHandler);

      cameraInitialized = true;
      cameraTexturePassed = true;
      // Pass camera texture info to native immediately
      nativeOnCameraTextureInitialized(nativeApp, textureId, cameraWidth, cameraHeight);
      Log.i(TAG, "Camera initialized with texture: " + textureId + " size: " + cameraWidth + "x" + cameraHeight);

    } catch (CameraAccessException e) {
      Log.e(TAG, "Camera access exception: " + e.getMessage());
    }
  }

  @Override
  protected void onDestroy() {
    super.onDestroy();
    // Release camera resources
    if (captureSession != null) {
      captureSession.close();
      captureSession = null;
    }
    if (cameraDevice != null) {
      cameraDevice.close();
      cameraDevice = null;
    }
    if (cameraSurface != null) {
      cameraSurface.release();
      cameraSurface = null;
    }
    if (cameraSurfaceTexture != null) {
      cameraSurfaceTexture.release();
      cameraSurfaceTexture = null;
    }
    if (cameraThread != null) {
      cameraThread.quitSafely();
      cameraThread = null;
    }
    cameraInitialized = false;
    cameraTexturePassed = false;
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

  private class Renderer implements GLSurfaceView.Renderer {
    @Override
    public void onSurfaceCreated(GL10 gl10, EGLConfig eglConfig) {
      nativeOnSurfaceCreated(nativeApp);

      // Initialize camera on GL thread if permission granted
      if (checkCameraPermission() && !cameraInitialized) {
        // Create camera texture in native code first
        int textureId = nativeCreateCameraTexture(nativeApp);
        initCameraOnGlThreadWithTexture(textureId);
      }
    }

    @Override
    public void onSurfaceChanged(GL10 gl10, int width, int height) {
      nativeSetScreenParams(nativeApp, width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl10) {
      // Update camera texture - must be called on GL thread
      if (cameraSurfaceTexture != null) {
        cameraSurfaceTexture.updateTexImage();
      }

      nativeOnDrawFrame(nativeApp);
    }
  }

  /** Callback for when close button is pressed. */
  public void closeSample(View view) {
    Log.d(TAG, "Leaving VR sample");
    finish();
  }

  /** Callback for when settings_menu button is pressed. */
  public void showSettings(View view) {
    PopupMenu popup = new PopupMenu(this, view);
    MenuInflater inflater = popup.getMenuInflater();
    inflater.inflate(R.menu.settings_menu, popup.getMenu());
    popup.setOnMenuItemClickListener(this);
    popup.show();
  }

  @Override
  public boolean onMenuItemClick(MenuItem item) {
    if (item.getItemId() == R.id.switch_viewer) {
      nativeSwitchViewer(nativeApp);
      return true;
    }
    return false;
  }

  /**
   * Checks for READ_EXTERNAL_STORAGE permission.
   *
   * @return whether the READ_EXTERNAL_STORAGE is already granted.
   */
  private boolean isReadExternalStorageEnabled() {
    return ActivityCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
        == PackageManager.PERMISSION_GRANTED;
  }

  /** Handles the requests for activity permission to READ_EXTERNAL_STORAGE. */
  private void requestPermissions() {
    final String[] permissions = new String[] {Manifest.permission.READ_EXTERNAL_STORAGE};
    ActivityCompat.requestPermissions(this, permissions, PERMISSIONS_REQUEST_CODE);
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
    if (requestCode == PERMISSIONS_REQUEST_CODE) {
      if (!isReadExternalStorageEnabled()) {
        Toast.makeText(this, R.string.read_storage_permission, Toast.LENGTH_LONG).show();
        if (!ActivityCompat.shouldShowRequestPermissionRationale(
            this, Manifest.permission.READ_EXTERNAL_STORAGE)) {
          launchPermissionsSettings();
        }
        finish();
      }
    } else if (requestCode == CAMERA_PERMISSIONS_REQUEST_CODE) {
      if (checkCameraPermission()) {
        // Camera will be initialized in onSurfaceCreated when GL context is ready
        Log.i(TAG, "Camera permission granted, will initialize on GL thread");
      } else {
        Toast.makeText(this, "Camera permission is required for passthrough", Toast.LENGTH_LONG).show();
      }
    }
  }

  private void launchPermissionsSettings() {
    Intent intent = new Intent();
    intent.setAction(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
    intent.setData(Uri.fromParts("package", getPackageName(), null));
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

  private native long nativeOnCreate(AssetManager assetManager);

  private native void nativeOnDestroy(long nativeApp);

  private native void nativeOnSurfaceCreated(long nativeApp);

  private native void nativeOnDrawFrame(long nativeApp);

  private native void nativeOnTriggerEvent(long nativeApp);

  private native void nativeOnPause(long nativeApp);

  private native void nativeOnResume(long nativeApp);

  private native void nativeSetScreenParams(long nativeApp, int width, int height);

  private native void nativeSwitchViewer(long nativeApp);

  private native void nativeOnCameraTextureInitialized(long nativeApp, int textureId, int width, int height);

  private native int nativeCreateCameraTexture(long nativeApp);

  public void updateCameraTexture() {
    if (cameraSurfaceTexture != null) {
      cameraSurfaceTexture.updateTexImage();
    }
  }
}
