package com.google.cardboard.core;
// Shared wire constants mirrored with the driver CardboardWire.h and the bridge net module; owns ports, message strings, and tags.
public final class AppConstants {
  private AppConstants() {}
  // Activity result codes for the storage and camera permission flows.
  public static final int PERMISSIONS_REQUEST_CODE = 2;
  public static final int CAMERA_PERMISSIONS_REQUEST_CODE = 3;
  // Locked UDP ports: video 42069, discovery 42070, telemetry 42071, camera 42072.
  public static final int VIDEO_PORT = 42069;
  public static final int UDP_DISCOVERY_PORT = 42070;
  public static final int TELEMETRY_PORT = 42071;
  public static final int CAMERA_PORT = 42072;
  // Discovery resend cadence while waiting for the driver ACK.
  public static final int DISCOVERY_INTERVAL_MS = 500;
  // Discovery/control strings exchanged with the driver on UDP 42070.
  public static final String DISCOVERY_MESSAGE = "CARDBOARD_DISCOVERY";
  public static final String DISCOVERY_ACK = "ACK";
  public static final String CAP_PREFIX = "CARDBOARD_CAP ";
  public static final String PHONE_HELLO = "CARDBOARD_PHONE_HELLO v1";
  public static final String PHONE_HELLO_PREFIX = "CARDBOARD_PHONE_HELLO v1";
  // Builds the telemetry hello with the app version suffix; called from TelemetrySender connect paths.
  public static String phoneHello(String version) {
    if (version == null || version.isEmpty()) return PHONE_HELLO;
    return PHONE_HELLO_PREFIX + " " + version;
  }
  // Keyframe demand string sent when the decoder stalls; consumed by the driver discovery path.
  public static final String KEYFRAME_REQ = "KEYFRAME_REQ";
  // Telemetry tags for UDP 42071: gyro 0x10, hand 0x11, rotation 0x12, netstats 0x13, ping 0x20.
  public static final byte TELEMETRY_TAG_GYRO = 0x10;
  public static final byte TELEMETRY_TAG_HAND = 0x11;
  public static final byte TELEMETRY_TAG_ROTATION = 0x12;
  public static final byte TELEMETRY_TAG_NETSTATS = 0x13;
  public static final byte TELEMETRY_TAG_PING = 0x20;
  // Camera capture floors and the downscaled stream shape sent to the bridge.
  public static final int DEFAULT_CAMERA_WIDTH = 640;
  public static final int DEFAULT_CAMERA_HEIGHT = 480;
  public static final int MIN_CAMERA_WIDTH = 640;
  public static final int MIN_CAMERA_HEIGHT = 480;
  public static final int CAMERA_STREAM_WIDTH = 256;
  public static final int CAMERA_STREAM_HEIGHT = 192;
  public static final int CAMERA_JPEG_QUALITY = 38;
  public static final long CAMERA_FRAME_INTERVAL_MS = 1000 / 30;
  // Camera datagram framing: 2-byte big-endian seq header, single-datagram size cap.
  public static final int CAMERA_SEQ_HEADER_LEN = 2;
  public static final int CAMERA_MAX_DATAGRAM = 60000;
  // Default decode surface size used before SPS parsing reports the stream size.
  public static final int DEFAULT_VIDEO_WIDTH = 2880;
  public static final int DEFAULT_VIDEO_HEIGHT = 1620;
}
