package com.google.cardboard.camera;
import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import java.util.ArrayList;
// Camera2 lifecycle owner in camera/; VrActivity drives open/pause/release and routes frames to CameraStreamer.
public class CameraController {
  private static final String TAG = CameraController.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  // Receiver for YUV frames; CameraStreamer implements it and VrActivity wires it via setFrameCallback.
  public interface FrameCallback {
    // Handles one camera image; returns true when ownership is taken, false when the caller closes it.
    boolean onFrame(Image image);
  }
  private final Context context;
  private final NativeBridge bridge;
  private CameraManager cameraManager;
  private CameraDevice cameraDevice;
  private CameraCaptureSession captureSession;
  private SurfaceTexture cameraSurfaceTexture;
  private Surface cameraSurface;
  private ImageReader imageReader;
  private HandlerThread cameraThread;
  private Handler cameraHandler;
  private boolean cameraInitialized = false;
  private boolean cameraTexturePassed = false;
  private final Object cameraLock = new Object();
  private FrameCallback frameCallback;
  private int cameraWidth = AppConstants.DEFAULT_CAMERA_WIDTH;
  private int cameraHeight = AppConstants.DEFAULT_CAMERA_HEIGHT;
  private int streamWidth = AppConstants.CAMERA_STREAM_WIDTH;
  private int streamHeight = AppConstants.CAMERA_STREAM_HEIGHT;
  // Stores the activity context and native bridge; called from VrActivity.onCreate on the UI thread.
  public CameraController(Context context, NativeBridge bridge) {
    this.context = context;
    this.bridge = bridge;
  }
  // Reports whether the OES texture reached native; called from VrActivity start and VrRenderer setup.
  public boolean isTexturePassed() {
    return cameraTexturePassed;
  }
  // Registers the frame consumer; called from VrActivity startSession with the CameraStreamer.
  public void setFrameCallback(FrameCallback callback) {
    this.frameCallback = callback;
  }
  // Creates the OES preview texture from the native id; called on the GL thread before openCamera.
  public void ensureCameraTexture(int textureId) {
    if (cameraSurfaceTexture == null) {
      cameraSurfaceTexture = new SurfaceTexture(textureId);
      cameraSurfaceTexture.setDefaultBufferSize(cameraWidth, cameraHeight);
      cameraSurface = new Surface(cameraSurfaceTexture);
      cameraTexturePassed = true;
      bridge.onCameraTextureInitialized(textureId, cameraWidth, cameraHeight);
      DBG.i("Camera texture created: %d", textureId);
    }
  }
  // Pushes the latest camera texel to the GL pipeline; called from VrRenderer.onDrawFrame on the GL thread.
  public void updateCameraTexture() {
    try {
      if (cameraSurfaceTexture != null) {
        cameraSurfaceTexture.updateTexImage();
      }
    } catch (Exception e) {
    }
  }
  // Opens the back camera and its handler thread; called from VrActivity startSession and VrRenderer setup.
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
      Size yuvChosen =
          CameraUtils.chooseYuvOutputSize(
              map, AppConstants.CAMERA_STREAM_WIDTH, AppConstants.CAMERA_STREAM_HEIGHT);
      streamWidth = yuvChosen.getWidth();
      streamHeight = yuvChosen.getHeight();
      if (cameraSurfaceTexture != null) {
        cameraSurfaceTexture.setDefaultBufferSize(cameraWidth, cameraHeight);
      }
      synchronized (cameraLock) {
        cameraThread = new HandlerThread("CameraThread");
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
      }
      cameraManager.openCamera(backCameraId, new CameraDeviceCallback(), cameraHandler);
      Log.i(TAG, "Camera opening, size: " + cameraWidth + "x" + cameraHeight
          + " stream: " + streamWidth + "x" + streamHeight);
    } catch (Exception e) {
      Log.w(TAG, "Could not open camera: " + e.getMessage());
    }
  }
  // Builds the preview plus YUV capture session; called from CameraDeviceCallback.onOpened on the camera thread.
  private void createCaptureSession() {
    if (cameraDevice == null || cameraSurface == null) {
      return;
    }
    try {
      imageReader =
          ImageReader.newInstance(
              streamWidth, streamHeight, ImageFormat.YUV_420_888,  2);
      imageReader.setOnImageAvailableListener(
          reader -> {
            Image image = reader.acquireLatestImage();
            if (image == null) return;
            boolean taken = false;
            try {
              if (frameCallback != null) {
                taken = frameCallback.onFrame(image);
              }
            } finally {
              if (!taken) {
                image.close();
              }
            }
          },
          cameraHandler);
      final CaptureRequest.Builder captureRequestBuilder =
          cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
      captureRequestBuilder.addTarget(cameraSurface);
      captureRequestBuilder.addTarget(imageReader.getSurface());
      try {
        String camId = cameraDevice.getId();
        CameraCharacteristics chars = cameraManager.getCameraCharacteristics(camId);
        Range<Integer>[] ranges =
            chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges != null && ranges.length > 0) {
          Range<Integer> best = null;
          for (Range<Integer> r : ranges) {
            if (r.getUpper() >= 30
                && (best == null || r.getLower() > best.getLower())) {
              best = r;
            }
          }
          if (best == null) {
            for (Range<Integer> r : ranges) {
              if (best == null || r.getUpper() > best.getUpper()) {
                best = r;
              }
            }
          }
          if (best != null) {
            captureRequestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, best);
            Log.i(TAG, "Camera fps range: " + best);
          }
        }
      } catch (Exception e) {
        Log.w(TAG, "FPS range not pinned: " + e.getMessage());
      }
      cameraDevice.createCaptureSession(
          new ArrayList<Surface>() {
            {
              add(cameraSurface);
              add(imageReader.getSurface());
            }
          },
          new PreviewSessionCallback(captureRequestBuilder),
          cameraHandler);
    } catch (Exception e) {
      Log.w(TAG, "Failed to create capture session: " + e.getMessage());
    }
  }
  // Tears down session, device, and GL textures; called from VrActivity.onPause on the UI thread.
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
    if (imageReader != null) {
      try {
        imageReader.close();
      } catch (Exception e) {
      }
      imageReader = null;
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
  // Releases all camera resources; called from VrActivity.onDestroy on the UI thread.
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
    if (imageReader != null) {
      imageReader.close();
      imageReader = null;
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
  // Device state listener started by openCamera; runs on the camera handler thread.
  private final class CameraDeviceCallback extends CameraDevice.StateCallback {
    // Stores the opened device and starts the session; invoked by Camera2 on the camera thread.
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
    // Clears the device reference on disconnect; invoked by Camera2 on the camera thread.
    @Override
    public void onDisconnected(CameraDevice camera) {
      synchronized (cameraLock) {
        camera.close();
        if (cameraDevice == camera) {
          cameraDevice = null;
        }
      }
    }
    // Clears the device reference on error; invoked by Camera2 on the camera thread.
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
  // Session state listener that starts the repeating preview request; runs on the camera handler thread.
  private final class PreviewSessionCallback extends CameraCaptureSession.StateCallback {
    private final CaptureRequest.Builder captureRequestBuilder;
    // Holds the preview request builder; called from createCaptureSession on the camera thread.
    PreviewSessionCallback(CaptureRequest.Builder captureRequestBuilder) {
      this.captureRequestBuilder = captureRequestBuilder;
    }
    // Stores the session and starts repeating preview; invoked by Camera2 on the camera thread.
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
    // Logs session configuration failure; invoked by Camera2 on the camera thread.
    @Override
    public void onConfigureFailed(CameraCaptureSession session) {
      Log.w(TAG, "Camera configuration failed");
    }
  }
}
