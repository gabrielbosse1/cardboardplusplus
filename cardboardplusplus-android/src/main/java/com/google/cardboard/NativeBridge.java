package com.google.cardboard;

/**
 * Abstraction over the JNI native methods declared in {@link VrActivity}.
 *
 * <p>The native method names are hard-bound to {@code Java_com_google_cardboard_VrActivity_*} in
 * the C++ layer, so the actual {@code native} declarations must stay in {@link VrActivity}. This
 * interface lets the extracted sub-packages invoke native behaviour without coupling to the
 * Activity class or to JNI internals, keeping those modules testable and the native surface in one
 * place.
 */
public interface NativeBridge {
  void onSurfaceCreated();

  void onDrawFrame();

  void onTriggerEvent();

  void onNativePause();

  void onNativeResume();

  void setScreenParams(int width, int height);

  void switchViewer();

  void onCameraTextureInitialized(int textureId, int width, int height);

  int createCameraTexture();

  int createVideoTexture();

  void setVideoDecoder(Object decoder);

  void onVideoActive();

  void resetCameraTexture();

  void setEyeTexture(int eye, int textureId);

  void startVideoReceiver(int port);

  void stopVideoReceiver();

  void updateVideoTexture();

  boolean hasVideoFrame();
}
