package com.google.cardboard.camera;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import android.view.Surface;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;
import java.util.ArrayList;

/**
 * Encapsulates all Camera2 lifecycle: texture creation, device open, capture session and teardown.
 * Texture creation runs on the GL thread; camera open/capture run on their own {@link
 * HandlerThread}.
 */
public class CameraController {
  private static final String TAG = CameraController.class.getSimpleName();

  private final Context context;
  private final NativeBridge bridge;

  private CameraManager cameraManager;
  private CameraDevice cameraDevice;
  private CameraCaptureSession captureSession;
  private SurfaceTexture cameraSurfaceTexture;
  private Surface cameraSurface;
  private HandlerThread cameraThread;
  private Handler cameraHandler;
  private boolean cameraInitialized = false;
  private boolean cameraTexturePassed = false;
  private final Object cameraLock = new Object();

  private int cameraWidth = AppConstants.DEFAULT_CAMERA_WIDTH;
  private int cameraHeight = AppConstants.DEFAULT_CAMERA_HEIGHT;

  public CameraController(Context context, NativeBridge bridge) {
    this.context = context;
    this.bridge = bridge;
  }

  public boolean isTexturePassed() {
    return cameraTexturePassed;
  }

  /** Creates the SurfaceTexture/Surface bound to the GL texture id and notifies native. */
  public void ensureCameraTexture(int textureId) {
    if (cameraSurfaceTexture == null) {
      cameraSurfaceTexture = new SurfaceTexture(textureId);
      cameraSurfaceTexture.setDefaultBufferSize(cameraWidth, cameraHeight);
      cameraSurface = new Surface(cameraSurfaceTexture);
      cameraTexturePassed = true;
      bridge.onCameraTextureInitialized(textureId, cameraWidth, cameraHeight);
      Log.i(TAG, "Camera texture created: " + textureId);
    }
  }

  public void updateCameraTexture() {
    try {
      if (cameraSurfaceTexture != null) {
        cameraSurfaceTexture.updateTexImage();
      }
    } catch (Exception e) {
      // Texture not ready or invalidated - skip frame, don't crash.
    }
  }

  @SuppressLint("MissingPermission")
  public void openCamera() {
    synchronized (cameraLock) {
      if (cameraInitialized) {
        return;
      }
    }

    cameraManager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
    if (cameraManager == null) {
      Log.w(TAG, "CameraManager not available");
      return;
    }

    try {
      String backCameraId = CameraUtils.findBackCameraId(cameraManager);
      if (backCameraId == null) {
        Log.w(TAG, "No back camera found");
        return;
      }

      CameraCharacteristics characteristics =
          cameraManager.getCameraCharacteristics(backCameraId);
      android.hardware.camera2.params.StreamConfigurationMap map =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      Size chosen =
          CameraUtils.chooseOutputSize(
              map, AppConstants.MIN_CAMERA_WIDTH, AppConstants.MIN_CAMERA_HEIGHT);
      cameraWidth = chosen.getWidth();
      cameraHeight = chosen.getHeight();

      if (cameraSurfaceTexture != null) {
        cameraSurfaceTexture.setDefaultBufferSize(cameraWidth, cameraHeight);
      }

      synchronized (cameraLock) {
        cameraThread = new HandlerThread("CameraThread");
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
      }

      cameraManager.openCamera(backCameraId, new CameraDeviceCallback(), cameraHandler);

      Log.i(TAG, "Camera opening, size: " + cameraWidth + "x" + cameraHeight);
    } catch (Exception e) {
      Log.w(TAG, "Could not open camera: " + e.getMessage());
    }
  }

  private void createCaptureSession() {
    if (cameraDevice == null || cameraSurface == null) {
      return;
    }
    try {
      final CaptureRequest.Builder captureRequestBuilder =
          cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
      captureRequestBuilder.addTarget(cameraSurface);

      cameraDevice.createCaptureSession(
          new ArrayList<Surface>() {
            {
              add(cameraSurface);
            }
          },
          new PreviewSessionCallback(captureRequestBuilder),
          cameraHandler);
    } catch (Exception e) {
      Log.w(TAG, "Failed to create capture session: " + e.getMessage());
    }
  }

  /**
   * Pauses the camera: resets the native texture and releases all hardware resources. Must be
   * called before the GL surface is paused.
   */
  public void onPause() {
    bridge.resetCameraTexture();

    synchronized (cameraLock) {
      cameraInitialized = false;
      if (captureSession != null) {
        try {
          captureSession.close();
        } catch (Exception e) {
        }
        captureSession = null;
      }
      if (cameraDevice != null) {
        try {
          cameraDevice.close();
        } catch (Exception e) {
        }
        cameraDevice = null;
      }
      if (cameraThread != null) {
        cameraThread.quitSafely();
        cameraThread = null;
      }
      cameraHandler = null;
    }

    if (cameraSurface != null) {
      try {
        cameraSurface.release();
      } catch (Exception e) {
      }
      cameraSurface = null;
    }
    if (cameraSurfaceTexture != null) {
      try {
        cameraSurfaceTexture.release();
      } catch (Exception e) {
      }
      cameraSurfaceTexture = null;
    }
    cameraTexturePassed = false;
  }

  /** Fully releases camera resources (used on activity destroy). */
  public void release() {
    synchronized (cameraLock) {
      if (captureSession != null) {
        captureSession.close();
        captureSession = null;
      }
      if (cameraDevice != null) {
        cameraDevice.close();
        cameraDevice = null;
      }
      if (cameraThread != null) {
        cameraThread.quitSafely();
        cameraThread = null;
      }
      cameraInitialized = false;
      cameraHandler = null;
    }
    if (cameraSurface != null) {
      cameraSurface.release();
      cameraSurface = null;
    }
    if (cameraSurfaceTexture != null) {
      cameraSurfaceTexture.release();
      cameraSurfaceTexture = null;
    }
    cameraTexturePassed = false;
  }

  /**
   * Carries the {@code openCamera} result back into the controller.
   *
   * <p>All state transitions are guarded by {@link #cameraLock}: only the first session set-up
   * wins (a second open is closed immediately), and any device closure clears {@link #cameraDevice}
   * so a later {@link #openCamera()} can retry.
   */
  private final class CameraDeviceCallback extends CameraDevice.StateCallback {
    @Override
    public void onOpened(CameraDevice camera) {
      synchronized (cameraLock) {
        if (!cameraInitialized) {
          cameraDevice = camera;
          try {
            createCaptureSession();
            cameraInitialized = true;
          } catch (Exception e) {
            Log.w(TAG, "Session setup failed: " + e.getMessage());
            camera.close();
            cameraDevice = null;
          }
        } else {
          camera.close();
        }
      }
    }

    @Override
    public void onDisconnected(CameraDevice camera) {
      synchronized (cameraLock) {
        camera.close();
        if (cameraDevice == camera) {
          cameraDevice = null;
        }
      }
    }

    @Override
    public void onError(CameraDevice camera, int error) {
      synchronized (cameraLock) {
        camera.close();
        if (cameraDevice == camera) {
          cameraDevice = null;
        }
      }
    }
  }

  /**
   * Starts the repeating preview stream once the capture session is configured.
   *
   * <p>Holds the {@link CaptureRequest.Builder} created alongside the session so the auto-focus and
   * auto-exposure settings the controller wants are applied when the stream starts. If the session
   * is configured after the camera was already torn down, it is closed immediately instead.
   */
  private final class PreviewSessionCallback extends CameraCaptureSession.StateCallback {
    private final CaptureRequest.Builder captureRequestBuilder;

    PreviewSessionCallback(CaptureRequest.Builder captureRequestBuilder) {
      this.captureRequestBuilder = captureRequestBuilder;
    }

    @Override
    public void onConfigured(CameraCaptureSession session) {
      synchronized (cameraLock) {
        if (!cameraInitialized || cameraDevice == null) {
          session.close();
          return;
        }
        captureSession = session;
      }
      try {
        captureRequestBuilder.set(
            CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
        captureRequestBuilder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
        session.setRepeatingRequest(captureRequestBuilder.build(), null, cameraHandler);
      } catch (Exception e) {
        Log.w(TAG, "Failed to start preview: " + e.getMessage());
      }
    }

    @Override
    public void onConfigureFailed(CameraCaptureSession session) {
      Log.w(TAG, "Camera configuration failed");
    }
  }
}
