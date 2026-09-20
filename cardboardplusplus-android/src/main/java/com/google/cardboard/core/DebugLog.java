package com.google.cardboard.core;
import android.util.Log;
import com.google.cardboard.BuildConfig;
// Per-tag gated logger; every manager owns one and VrActivity toggles the global switch from settings.
public final class DebugLog {
    private final String tag;
    private volatile boolean enabled;
    // Creates a logger for the given log tag; called from each owning class field initializer.
    public DebugLog(String tag) {
        this.tag = tag;
    }
    // Sets the instance-level gate; called from tests and ad-hoc per-class enabling.
    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
    }
    // Reports the instance-level gate; called from TelemetrySender start logging.
    public boolean isEnabled() {
        return enabled;
    }
    private static volatile boolean globalEnabled = false;
    // Flips the app-wide gate; called from VrActivity lifecycle and SettingsMenuController toggle.
    public static void setGlobalEnabled(boolean enabled) {
        globalEnabled = enabled;
    }
    // Debug output passes in debug builds or when either gate is on; called from all d/i/v helpers.
    private boolean isEnabledGlobal() {
        return BuildConfig.DEBUG || enabled || globalEnabled;
    }
    // Emits a debug line when gates allow; called from hot paths across managers.
    public void d(String msg) {
        if (isEnabledGlobal()) Log.d(tag, msg);
    }
    // Emits a formatted debug line; called from DiscoveryManager and sensor enumeration.
    public void d(String fmt, Object... args) {
        if (isEnabledGlobal()) Log.d(tag, String.format(fmt, args));
    }
    // Emits an info line when gates allow; called for lifecycle and capability reports.
    public void i(String msg) {
        if (isEnabledGlobal()) Log.i(tag, msg);
    }
    // Emits a formatted info line; called for decoder caps, fps, and streamer stats.
    public void i(String fmt, Object... args) {
        if (isEnabledGlobal()) Log.i(tag, String.format(fmt, args));
    }
    // Emits a verbose line when gates allow; called from low-volume diagnostics.
    public void v(String msg) {
        if (isEnabledGlobal()) Log.v(tag, msg);
    }
    // Emits a formatted verbose line; called from low-volume diagnostics.
    public void v(String fmt, Object... args) {
        if (isEnabledGlobal()) Log.v(tag, String.format(fmt, args));
    }
    // Always emits a warning; called from sensor-missing and throttled-failure paths.
    public void w(String msg) {
        Log.w(tag, msg);
    }
    // Always emits a warning with throwable; called from paths that carry an exception.
    public void w(String msg, Throwable t) {
        Log.w(tag, msg, t);
    }
    // Always emits an error; called from unrecoverable per-component failures.
    public void e(String msg) {
        Log.e(tag, msg);
    }
    // Always emits an error with throwable; called from paths that carry an exception.
    public void e(String msg, Throwable t) {
        Log.e(tag, msg, t);
    }
}
