package com.google.cardboard.codec;

/**
 * Supported video codecs for the PC -> phone stream.
 *
 * <p>{@link #fromName} accepts either the wire name (e.g. {@code "h264"}) or the enum constant name
 * ({@code "H264"}) and falls back to {@link #H264} for anything unknown so legacy/broken settings
 * always resolve to the only codec the native decoder currently implements.
 */
public enum VideoCodec {
  H264("h264"),
  HEVC("hevc"),
  AV1("av1");

  private final String codecName;

  VideoCodec(String codecName) {
    this.codecName = codecName;
  }

  public String codecName() {
    return codecName;
  }

  public static VideoCodec fromName(String value) {
    if (value != null) {
      for (VideoCodec c : values()) {
        if (c.codecName.equalsIgnoreCase(value) || c.name().equalsIgnoreCase(value)) {
          return c;
        }
      }
    }
    return H264;
  }
}
