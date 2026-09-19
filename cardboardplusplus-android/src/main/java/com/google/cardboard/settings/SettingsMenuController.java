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
import android.widget.Toast;
import com.google.cardboard.NativeBridge;
import com.google.cardboard.R;
import com.google.cardboard.core.DebugLog;

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
    if (item.getItemId() == R.id.toggle_debug) {
      toggleDebugLogging();
      return true;
    }
    return false;
  }

  private void toggleDebugLogging() {
    boolean newState = !appSettings.isDebugLogging();
    appSettings.setDebugLogging(newState);
    DebugLog.setGlobalEnabled(newState);
    Toast.makeText(
        anchor.getContext(),
        newState ? R.string.debug_on : R.string.debug_off,
        Toast.LENGTH_SHORT)
        .show();
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
            (dialog, which) -> {
              String ip = ipInput.getText().toString().trim();
              // Empty clears back to auto-discovery; anything else must parse
              // (never persist garbage that would black-hole discovery).
              if (!ip.isEmpty() && !isValidIp(ip)) {
                Toast.makeText(context, R.string.pc_ip_invalid, Toast.LENGTH_LONG).show();
                return;
              }
              appSettings.setPcIp(ip);
            })
        .setNegativeButton(R.string.pc_ip_cancel, null)
        .show();
  }

  /** True when the string resolves to an IP address (v4 preferred, v6 accepted). */
  static boolean isValidIp(String ip) {
    try {
      java.net.InetAddress.getByName(ip);
      return true;
    } catch (Exception e) {
      return false;
    }
  }
}
