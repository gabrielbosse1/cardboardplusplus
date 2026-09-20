package com.google.cardboard.ui;
import android.os.Build;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
// Fullscreen helper in ui/; VrActivity applies it on create, focus gain, and system-UI visibility changes.
public final class ImmersiveMode {
  private ImmersiveMode() {}
  // Returns the legacy sticky-immersive flag set; called from applySticky on pre-R devices.
  public static int stickySystemUiFlags() {
    return View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_FULLSCREEN
        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
  }
  // Hides status and navigation bars with swipe-back behavior; called from VrActivity create/focus/visibility paths.
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
