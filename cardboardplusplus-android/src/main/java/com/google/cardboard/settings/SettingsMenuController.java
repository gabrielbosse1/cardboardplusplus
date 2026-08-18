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
 * Owns the settings popup menu (switch viewer / set PC IP).
 *
 * <p>Binds the inflated {@code settings_menu} items to their actions. This is deliberately detached
 * from {@code VrActivity} so adding a future menu item only touches this class plus the menu XML.
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
    EditText ipInput = new EditText(context);
    ipInput.setInputType(
        InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_VARIATION_URI
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
    ipInput.setHint(R.string.pc_ip_hint);
    ipInput.setText(appSettings.getPcIp());
    ipInput.setSelectAllOnFocus(true);
    ipInput.setImeOptions(EditorInfo.IME_ACTION_DONE);

    new AlertDialog.Builder(context)
        .setTitle(R.string.pc_ip_dialog_title)
        .setView(ipInput)
        .setPositiveButton(
            R.string.pc_ip_ok,
            (dialog, which) -> appSettings.setPcIp(ipInput.getText().toString().trim()))
        .setNegativeButton(R.string.pc_ip_cancel, null)
        .show();
  }
}
