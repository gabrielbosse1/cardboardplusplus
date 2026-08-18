package com.google.cardboard.ui;

import android.view.View;
import android.view.Window;

/**
 * Owns the "sticky immersive" fullscreen mode used by the VR activity.
 *
 * <p>Cardboard apps must hide the system bars so the stereo view fills the screen and the
 * controller (magnet/button/touch) stays primary. Extracted from {@code VrActivity} so the full set
 * of flags lives in one obviously-named place instead of inside the activity's lifecycle wiring.
 */
public final class ImmersiveMode {
  private ImmersiveMode() {}

  /** The flag mask for full-content, no-dimming immersive mode (Android 4.4+ API 19). */
  public static int stickySystemUiFlags() {
    return View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_FULLSCREEN
        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
  }

  /** Applies sticky immersive mode to the given window's decor view. */
  public static void applySticky(Window window) {
    window.getDecorView().setSystemUiVisibility(stickySystemUiFlags());
  }
}