package com.google.cardboard.render;

import android.opengl.GLSurfaceView;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.camera.CameraController;
import com.google.cardboard.permissions.PermissionManager;
import com.google.cardboard.video.VideoManager;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** GLSurfaceView renderer: drives native draw, the video receiver and camera passthrough. */
public class VrRenderer implements GLSurfaceView.Renderer {
  private static final String TAG = VrRenderer.class.getSimpleName();

  private final NativeBridge bridge;
  private final CameraController cameraController;
  private final VideoManager videoManager;
  private final PermissionManager permissionManager;

  private final FpsCounter fpsCounter = new FpsCounter(TAG);

  public VrRenderer(
      NativeBridge bridge,
      CameraController cameraController,
      VideoManager videoManager,
      PermissionManager permissionManager) {
    this.bridge = bridge;
    this.cameraController = cameraController;
    this.videoManager = videoManager;
    this.permissionManager = permissionManager;
  }

  @Override
  public void onSurfaceCreated(GL10 gl10, EGLConfig eglConfig) {
    bridge.onSurfaceCreated();

    videoManager.onSurfaceCreated();
    videoManager.start();

    if (permissionManager.isCameraGranted() && !cameraController.isTexturePassed()) {
      int textureId = bridge.createCameraTexture();
      cameraController.ensureCameraTexture(textureId);
      cameraController.openCamera();
    }
  }

  @Override
  public void onSurfaceChanged(GL10 gl10, int width, int height) {
    bridge.setScreenParams(width, height);
  }

  @Override
  public void onDrawFrame(GL10 gl10) {
    fpsCounter.onFrameRendered();
    cameraController.updateCameraTexture();
    videoManager.updateTexture();
    bridge.onDrawFrame();
  }
}
