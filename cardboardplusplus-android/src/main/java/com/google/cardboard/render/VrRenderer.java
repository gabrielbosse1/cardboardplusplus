package com.google.cardboard.render;
import android.opengl.GLSurfaceView;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.camera.CameraController;
import com.google.cardboard.permissions.PermissionManager;
import com.google.cardboard.video.VideoManager;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
// GL lifecycle owner: forwards surface events to the native Cardboard SDK
// (bridge), starts video decode once GL exists, and opens the camera only
// after permission with a native texture. Constructed by VrActivity.
public class VrRenderer implements GLSurfaceView.Renderer {
  private static final String TAG = VrRenderer.class.getSimpleName();
  private final NativeBridge bridge;
  private final CameraController cameraController;
  private final VideoManager videoManager;
  private final PermissionManager permissionManager;
  private final FpsCounter fpsCounter = new FpsCounter(TAG);
  // Wires the collaborators VrActivity provides; nothing starts here (GL
  // thread callbacks below drive startup).
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
  // GL ready: inits native SDK + video, starts decode, and opens the camera
  // with a native texture when permitted (passthrough background).
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
  // Forwards the surface size so the native SDK builds correct eye matrices.
  @Override
  public void onSurfaceChanged(GL10 gl10, int width, int height) {
    bridge.setScreenParams(width, height);
  }
  // Per-frame pump: FPS tick, fresh camera + video textures, then the native
  // draw (lens distortion + SBS present).
  @Override
  public void onDrawFrame(GL10 gl10) {
    fpsCounter.onFrameRendered();
    cameraController.updateCameraTexture();
    videoManager.updateTexture();
    bridge.onDrawFrame();
  }
}
