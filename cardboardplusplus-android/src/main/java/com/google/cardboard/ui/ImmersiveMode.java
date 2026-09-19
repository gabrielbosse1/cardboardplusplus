package com.google.cardboard.ui;

import android.os.Build;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/**
 * Owns the "sticky immersive" fullscreen mode used by the VR activity.
 *
 * <p>Cardboard apps must hide the system bars so the stereo view fills the screen and the
 * controller (magnet/button/touch) stays primary. Extracted from {@code VrActivity} so the full set
 * of flags lives in one obviously-named place instead of inside the activity's lifecycle wiring.
 *
 * <p>API 30+ uses WindowInsetsController (the SYSTEM_UI_FLAG_* path is deprecated
 * but kept as the fallback for older devices); behaviour is identical — sticky
 * immersive with layout under the bars.
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
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
      window.setDecorFitsSystemWindows(false);
      WindowInsetsController controller = window.getInsetsController();
      if (controller != null) {
        controller.hide(
            WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
        controller.setSystemBarsBehavior(
            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        return;
      }
    }
    window.getDecorView().setSystemUiVisibility(stickySystemUiFlags());
  }
}
