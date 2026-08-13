package com.google.cardboard.codec;

/** Supported video codecs for the PC -> phone stream. */
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
