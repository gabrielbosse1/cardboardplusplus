package com.google.cardboard.core;

/** App-wide constants: ports, intervals, permission request codes and default sizes. */
public final class AppConstants {
  private AppConstants() {}

  // Permission request codes
  public static final int PERMISSIONS_REQUEST_CODE = 2;
  public static final int CAMERA_PERMISSIONS_REQUEST_CODE = 3;

  // Network ports
  public static final int UDP_DISCOVERY_PORT = 42070;
  public static final int VIDEO_PORT = 42069;
  public static final int DISCOVERY_INTERVAL_MS = 500;

  // Camera defaults
  public static final int DEFAULT_CAMERA_WIDTH = 640;
  public static final int DEFAULT_CAMERA_HEIGHT = 480;
  public static final int MIN_CAMERA_WIDTH = 640;
  public static final int MIN_CAMERA_HEIGHT = 480;

  // Default video stream dimensions (matches native defaults)
  public static final int DEFAULT_VIDEO_WIDTH = 1920;
  public static final int DEFAULT_VIDEO_HEIGHT = 1080;
}
