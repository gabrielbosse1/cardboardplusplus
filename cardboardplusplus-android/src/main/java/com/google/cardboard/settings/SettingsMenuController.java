package com.google.cardboard.settings;

import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
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
  private final AppSettings appSettings;

  public SettingsMenuController(View anchor, NativeBridge bridge, AppSettings appSettings) {
    this.anchor = anchor;
    this.bridge = bridge;
    this.appSettings = appSettings;
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
    if (item.getItemId() == R.id.set_pc_ip) {
      showPcIpDialog();
      return true;
    }
    return false;
  }

  private void showPcIpDialog() {
    Context context = anchor.getContext();
    EditText input = new EditText(context);
    input.setInputType(
        InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_VARIATION_URI
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
    input.setHint(R.string.pc_ip_hint);
    input.setText(appSettings.getPcIp());
    input.setSelectAllOnFocus(true);
    input.setImeOptions(EditorInfo.IME_ACTION_DONE);

    new AlertDialog.Builder(context)
        .setTitle(R.string.pc_ip_dialog_title)
        .setView(input)
        .setPositiveButton(
            R.string.pc_ip_ok,
            (dialog, which) -> appSettings.setPcIp(input.getText().toString().trim()))
        .setNegativeButton(R.string.pc_ip_cancel, null)
        .show();
  }
}
