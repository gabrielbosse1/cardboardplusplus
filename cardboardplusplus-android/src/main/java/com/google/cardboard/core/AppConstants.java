package com.google.cardboard.core;

/** App-wide constants: ports, intervals, permission request codes and default sizes. */
public final class AppConstants {
  private AppConstants() {}

  // Permission request codes
  public static final int PERMISSIONS_REQUEST_CODE = 2;
  public static final int CAMERA_PERMISSIONS_REQUEST_CODE = 3;

  // Network ports (locked wire values — must match driver CardboardWire.h;
  // centralize here, never change the values).
  public static final int VIDEO_PORT = 42069;
  public static final int UDP_DISCOVERY_PORT = 42070;
  public static final int TELEMETRY_PORT = 42071;
  public static final int CAMERA_PORT = 42072;
  public static final int DISCOVERY_INTERVAL_MS = 500;

  // Wire protocol strings (locked — shared with the PC driver, change only
  // together with the driver side, see driver CardboardWire.h).
  public static final String DISCOVERY_MESSAGE = "CARDBOARD_DISCOVERY";
  public static final String DISCOVERY_ACK = "ACK";
  public static final String CAP_PREFIX = "CARDBOARD_CAP ";
  public static final String PHONE_HELLO = "CARDBOARD_PHONE_HELLO v1";
  /** Hello prefix without the build version (see {@link #phoneHello}). */
  public static final String PHONE_HELLO_PREFIX = "CARDBOARD_PHONE_HELLO v1";
  /**
   * Full hello the phone sends on first contact: {@code CARDBOARD_PHONE_HELLO v1 <version>}
   * where version is the commit count (BuildConfig.VERSION_NAME). The bridge only checks
   * the prefix, so old bridges still accept it.
   */
  public static String phoneHello(String version) {
    if (version == null || version.isEmpty()) return PHONE_HELLO;
    return PHONE_HELLO_PREFIX + " " + version;
  }
  // Phone -> driver loss recovery on UDP_DISCOVERY_PORT: forces the next
  // encoded frame to IDR. Handled with no ACK and no target switch.
  public static final String KEYFRAME_REQ = "KEYFRAME_REQ";

  // Telemetry tags on TELEMETRY_PORT (locked wire values, do not change).
  public static final byte TELEMETRY_TAG_GYRO = 0x10;
  public static final byte TELEMETRY_TAG_HAND = 0x11;
  public static final byte TELEMETRY_TAG_ROTATION = 0x12;
  public static final byte TELEMETRY_TAG_NETSTATS = 0x13;
  public static final byte TELEMETRY_TAG_PING = 0x20;

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

  // Default video stream dimensions: single source of truth for the fallback
  // size (matches the native receiver defaults; the SPS-coded size overrides
  // this at runtime once keyframes arrive).
  public static final int DEFAULT_VIDEO_WIDTH = 2880;
  public static final int DEFAULT_VIDEO_HEIGHT = 1620;
}
