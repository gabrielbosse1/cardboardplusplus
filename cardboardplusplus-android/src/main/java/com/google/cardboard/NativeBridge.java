package com.google.cardboard;
// JNI boundary VrActivity implements; VrRenderer, VideoManager, and CameraController call through it on the GL thread.
public interface NativeBridge {
  // Forwards surface creation to the native Cardboard SDK; called from VrRenderer.onSurfaceCreated on the GL thread.
  void onSurfaceCreated();
  // Drives one native frame render; called from VrRenderer.onDrawFrame on the GL thread.
  void onDrawFrame();
  // Delivers a screen-tap trigger to native; called from VrActivity's touch listener via queueEvent.
  void onTriggerEvent();
  // Pauses native rendering; called from VrActivity.onPause on the UI thread.
  void onNativePause();
  // Resumes native rendering; called from VrActivity start paths on the UI thread.
  void onNativeResume();
  // Passes the GL surface size in pixels; called from VrRenderer.onSurfaceChanged on the GL thread.
  void setScreenParams(int width, int height);
  // Opens the native viewer-profile switcher; called from SettingsMenuController menu handling.
  void switchViewer();
  // Hands the camera OES texture to native passthrough; called from CameraController.ensureCameraTexture.
  void onCameraTextureInitialized(int textureId, int width, int height);
  // Allocates the camera OES texture id; called from VrActivity start and VrRenderer setup on the GL thread.
  int createCameraTexture();
  // Allocates the video OES texture id; called from VideoManager.onSurfaceCreated on the GL thread.
  int createVideoTexture();
  // Registers the MediaCodec-backed decoder with native; called from VideoManager.onSurfaceCreated.
  void setVideoDecoder(Object decoder);
  // Signals first decoded video output; called from VideoDecoder once MediaCodec is configured.
  void onVideoActive();
  // Corrects the video UV range for padded decode height; called from VideoDecoder after configure.
  void setVideoVMax(float vMax);
  // Clears the camera texture binding; called from CameraController.onPause.
  void resetCameraTexture();
  // Starts the native UDP video receiver on the given port; called from VideoManager.start.
  void startVideoReceiver(int port);
  // Stops the native UDP video receiver; called from VideoManager.onPause.
  void stopVideoReceiver();
}
