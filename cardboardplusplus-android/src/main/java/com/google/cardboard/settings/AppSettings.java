package com.google.cardboard.settings;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Phone-side settings that the app itself owns: PC IP and debug logging.
 *
 * <p>Streaming policy (resolution, frame rate, bitrate, codec) is owned by the
 * bridge/driver: the phone announces its hardware decode ceiling via
 * CARDBOARD_CAP and the PC clamps the encoder. The stream is AVC-only, so no
 * codec preference is stored here.
 */
public class AppSettings {
  private static final String PREFS_NAME = "cardboard_plusplus_settings";
  private static final String KEY_PC_IP = "pc_ip";
  private static final String KEY_DEBUG_LOGGING = "debug_logging";

  private final SharedPreferences prefs;

  private String pcIp;
  private boolean debugLogging;

  public AppSettings(Context context) {
    this.prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    load();
  }

  private void load() {
    pcIp = prefs.getString(KEY_PC_IP, "");
    debugLogging = prefs.getBoolean(KEY_DEBUG_LOGGING, false);
  }

  /** PC driver IP for direct (non-broadcast) discovery. Empty = auto-discovery. */
  public String getPcIp() {
    return pcIp;
  }

  public void setPcIp(String ip) {
    pcIp = (ip == null) ? "" : ip.trim();
    prefs.edit().putString(KEY_PC_IP, pcIp).apply();
  }

  /** Whether verbose debug logging is enabled. Off by default to avoid log spam. */
  public boolean isDebugLogging() {
    return debugLogging;
  }

  public void setDebugLogging(boolean enabled) {
    debugLogging = enabled;
    prefs.edit().putBoolean(KEY_DEBUG_LOGGING, enabled).apply();
  }
}
