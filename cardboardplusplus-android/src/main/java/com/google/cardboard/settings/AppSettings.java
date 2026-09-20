package com.google.cardboard.settings;
import android.content.Context;
import android.content.SharedPreferences;
// Persisted user prefs (SharedPreferences "cardboard_plusplus_settings"):
// the PC override IP read by telemetry/camera/discovery, and the debug-log
// toggle. Cached in memory; setters persist immediately.
public class AppSettings {
  private static final String PREFS_NAME = "cardboard_plusplus_settings";
  private static final String KEY_PC_IP = "pc_ip";
  private static final String KEY_DEBUG_LOGGING = "debug_logging";
  private final SharedPreferences prefs;
  private String pcIp;
  private boolean debugLogging;
  // Loads cached values (empty IP, debug off) at construction.
  public AppSettings(Context context) {
    this.prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    load();
  }
  // Reads stored values into the cache. Missing keys yield the defaults.
  private void load() {
    pcIp = prefs.getString(KEY_PC_IP, "");
    debugLogging = prefs.getBoolean(KEY_DEBUG_LOGGING, false);
  }
  // PC override IP ("" = auto/broadcast). Read on every telemetry send, so
  // the settings dialog takes effect without restarting the stream.
  public String getPcIp() {
    return pcIp;
  }
  // Stores the trimmed IP (null becomes ""). Persists immediately.
  public void setPcIp(String ip) {
    pcIp = (ip == null) ? "" : ip.trim();
    prefs.edit().putString(KEY_PC_IP, pcIp).apply();
  }
  // Verbose-logging toggle consulted by DebugLog on every call.
  public boolean isDebugLogging() {
    return debugLogging;
  }
  // Persists the toggle immediately; takes effect on the next log call.
  public void setDebugLogging(boolean enabled) {
    debugLogging = enabled;
    prefs.edit().putBoolean(KEY_DEBUG_LOGGING, enabled).apply();
  }
}
