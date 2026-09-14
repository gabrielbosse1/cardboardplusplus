package com.google.cardboard.core;

import android.util.Log;
import com.google.cardboard.settings.AppSettings;

/**
 * Conditional debug logging. Every verbose log call in the app goes through
 * here; when debug is off, the calls compile away to a boolean check.
 *
 * <p>Usage:
 * <pre>
 *   private static final DebugLog DBG = new DebugLog("MyTag");
 *   DBG.d("sensor values: %.3f, %.3f, %.3f", x, y, z);
 * </pre>
 *
 * <p>Normal {@link Log#w} and {@link Log#e} calls are NOT gated — warnings
 * and errors always fire regardless of the debug flag.
 */
public final class DebugLog {
    private final String tag;
    private volatile boolean enabled;

    public DebugLog(String tag) {
        this.tag = tag;
    }

    /** Call once per session or when the user toggles the setting. */
    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
        globalEnabled = enabled;
    }

    public boolean isEnabled() {
        return enabled;
    }

    // Global flag so DebugLog instances created without AppSettings still work.
    private static volatile boolean globalEnabled = false;

    /** Set the global debug flag. Called from VrActivity on create/resume. */
    public static void setGlobalEnabled(boolean enabled) {
        globalEnabled = enabled;
    }

    /** Check the global debug flag (used by instances without explicit AppSettings). */
    private boolean isEnabledGlobal() {
        return enabled || globalEnabled;
    }

    /** Debug-level log. Only fires when debug is enabled. */
    public void d(String msg) {
        if (isEnabledGlobal()) Log.d(tag, msg);
    }

    /** Debug-level log with format args. Only fires when debug is enabled. */
    public void d(String fmt, Object... args) {
        if (isEnabledGlobal()) Log.d(tag, String.format(fmt, args));
    }

    /** Info-level log. Only fires when debug is enabled. */
    public void i(String msg) {
        if (isEnabledGlobal()) Log.i(tag, msg);
    }

    /** Info-level log with format args. Only fires when debug is enabled. */
    public void i(String fmt, Object... args) {
        if (isEnabledGlobal()) Log.i(tag, String.format(fmt, args));
    }

    /** Verbose-level log. Only fires when debug is enabled. */
    public void v(String msg) {
        if (isEnabledGlobal()) Log.v(tag, msg);
    }

    /** Verbose-level log with format args. Only fires when debug is enabled. */
    public void v(String fmt, Object... args) {
        if (isEnabledGlobal()) Log.v(tag, String.format(fmt, args));
    }

    // --- Always-on helpers (not gated) ---

    /** Warning log — always fires regardless of debug flag. */
    public void w(String msg) {
        Log.w(tag, msg);
    }

    /** Warning log with throwable — always fires. */
    public void w(String msg, Throwable t) {
        Log.w(tag, msg, t);
    }

    /** Error log — always fires regardless of debug flag. */
    public void e(String msg) {
        Log.e(tag, msg);
    }

    /** Error log with throwable — always fires. */
    public void e(String msg, Throwable t) {
        Log.e(tag, msg, t);
    }

    /**
     * Create a DebugLog for the given class, reading the initial enabled
     * state from AppSettings.
     */
    public static DebugLog create(Class<?> clazz, AppSettings settings) {
        DebugLog log = new DebugLog(clazz.getSimpleName());
        if (settings != null) log.setEnabled(settings.isDebugLogging());
        return log;
    }
}
