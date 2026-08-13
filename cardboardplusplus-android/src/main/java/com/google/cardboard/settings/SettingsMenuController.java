package com.google.cardboard.settings;

import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.widget.PopupMenu;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.R;

/**
 * Owns the settings popup menu. Currently only "switch viewer" is implemented; the menu is the
 * natural home for the planned resolution / frame-rate / internet-speed / codec selectors.
 */
public class SettingsMenuController implements PopupMenu.OnMenuItemClickListener {
  private final View anchor;
  private final NativeBridge bridge;

  public SettingsMenuController(View anchor, NativeBridge bridge) {
    this.anchor = anchor;
    this.bridge = bridge;
  }

  public void show() {
    PopupMenu popup = new PopupMenu(anchor.getContext(), anchor);
    MenuInflater inflater = popup.getMenuInflater();
    inflater.inflate(R.menu.settings_menu, popup.getMenu());
    popup.setOnMenuItemClickListener(this);
    popup.show();
  }

  @Override
  public boolean onMenuItemClick(MenuItem item) {
    if (item.getItemId() == R.id.switch_viewer) {
      bridge.switchViewer();
      return true;
    }
    return false;
  }
}
