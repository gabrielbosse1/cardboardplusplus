package com.google.cardboard.settings;

import android.content.Context;
import android.content.SharedPreferences;
import com.google.cardboard.codec.VideoCodec;

/**
 * User-configurable streaming settings (resolution, frame rate, bitrate/internet speed and
 * preferred codec). Backed by {@link SharedPreferences} so they survive restarts and can be edited
 * from the settings menu. Defaults match the current native behaviour.
 */
public class AppSettings {
  private static final String PREFS_NAME = "cardboard_plusplus_settings";
  private static final String KEY_VIDEO_WIDTH = "video_width";
  private static final String KEY_VIDEO_HEIGHT = "video_height";
  private static final String KEY_FRAME_RATE = "frame_rate";
  private static final String KEY_BITRATE_KBPS = "bitrate_kbps";
  private static final String KEY_CODEC = "codec";

  private final SharedPreferences prefs;

  private int videoWidth;
  private int videoHeight;
  private int frameRate;
  private int bitrateKbps;
  private VideoCodec codec;

  public AppSettings(Context context) {
    this.prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    load();
  }

  private void load() {
    videoWidth = prefs.getInt(KEY_VIDEO_WIDTH, 2880);
    videoHeight = prefs.getInt(KEY_VIDEO_HEIGHT, 1620);
    frameRate = prefs.getInt(KEY_FRAME_RATE, 60);
    bitrateKbps = prefs.getInt(KEY_BITRATE_KBPS, 20000);
    codec = VideoCodec.fromName(prefs.getString(KEY_CODEC, VideoCodec.H264.name()));
  }

  public int getVideoWidth() {
    return videoWidth;
  }

  public int getVideoHeight() {
    return videoHeight;
  }

  public int getFrameRate() {
    return frameRate;
  }

  public int getBitrateKbps() {
    return bitrateKbps;
  }

  public VideoCodec getCodec() {
    return codec;
  }

  public void setVideoResolution(int width, int height) {
    videoWidth = width;
    videoHeight = height;
    prefs.edit().putInt(KEY_VIDEO_WIDTH, width).putInt(KEY_VIDEO_HEIGHT, height).apply();
  }

  public void setFrameRate(int fps) {
    frameRate = fps;
    prefs.edit().putInt(KEY_FRAME_RATE, fps).apply();
  }

  public void setBitrateKbps(int kbps) {
    bitrateKbps = kbps;
    prefs.edit().putInt(KEY_BITRATE_KBPS, kbps).apply();
  }

  public void setCodec(VideoCodec c) {
    codec = c;
    prefs.edit().putString(KEY_CODEC, c.name()).apply();
  }
}
