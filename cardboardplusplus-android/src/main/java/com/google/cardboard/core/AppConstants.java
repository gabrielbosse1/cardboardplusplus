package com.google.cardboard.core;

/** App-wide constants: ports, intervals, permission request codes and default sizes. */
public final class AppConstants {
  private AppConstants() {}

  // Permission request codes
  public static final int PERMISSIONS_REQUEST_CODE = 2;
  public static final int CAMERA_PERMISSIONS_REQUEST_CODE = 3;

  // Network ports
  public static final int VIDEO_PORT = 42069;
  public static final int UDP_DISCOVERY_PORT = 42070;
  public static final int TELEMETRY_PORT = 42071;
  public static final int CAMERA_PORT = 42072;
  public static final int DISCOVERY_INTERVAL_MS = 500;

  // Wire protocol strings (shared with the PC driver) - change only together
  // with the driver side (see driver CardboardWire.h).
  // Phone -> driver loss recovery on UDP_DISCOVERY_PORT: forces the next
  // encoded frame to IDR. Handled with no ACK and no target switch.
  public static final String KEYFRAME_REQ = "KEYFRAME_REQ";

  // Camera defaults
  public static final int DEFAULT_CAMERA_WIDTH = 640;
  public static final int DEFAULT_CAMERA_HEIGHT = 480;
  public static final int MIN_CAMERA_WIDTH = 640;
  public static final int MIN_CAMERA_HEIGHT = 480;

  // Camera stream wire format (phone -> bridge on CAMERA_PORT, UDP 42072).
  // 256x192 matches MediaPipe's internal scale (~224-256px); anything bigger
  // is downscaled inside the model anyway. Single JPEG per datagram.
  public static final int CAMERA_STREAM_WIDTH = 256;
  public static final int CAMERA_STREAM_HEIGHT = 192;
  public static final int CAMERA_JPEG_QUALITY = 38;
  public static final long CAMERA_FRAME_INTERVAL_MS = 1000 / 30; // 30 fps
  public static final int CAMERA_SEQ_HEADER_LEN = 2; // u16 seq BE + JPEG
  public static final int CAMERA_MAX_DATAGRAM = 60000;

  // Default video stream dimensions (matches native defaults)
  public static final int DEFAULT_VIDEO_WIDTH = 1920;
  public static final int DEFAULT_VIDEO_HEIGHT = 1080;
}
