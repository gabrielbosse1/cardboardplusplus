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
// Overflow-menu controller (viewer switch, PC IP, debug toggle): builds the
// PopupMenu on an anchor view and routes each item. Owned by VrActivity.
public class SettingsMenuController implements PopupMenu.OnMenuItemClickListener {
  private final View anchor;
  private final NativeBridge bridge;
  private final AppSettings appSettings;
  // Captures the anchor view, native SDK handle, and prefs. Nothing shows
  // until show() is called.
  public SettingsMenuController(View anchor, NativeBridge bridge, AppSettings appSettings) {
    this.anchor = anchor;
    this.bridge = bridge;
    this.appSettings = appSettings;
  }
  // Inflates and shows the settings popup anchored to the view.
  public void show() {
    PopupMenu popup = new PopupMenu(anchor.getContext(), anchor);
    MenuInflater inflater = popup.getMenuInflater();
    inflater.inflate(R.menu.settings_menu, popup.getMenu());
    popup.setOnMenuItemClickListener(this);
    popup.show();
  }
  // Routes the tapped item: viewer switch via native SDK, PC-IP dialog, or
  // debug toggle. False for unknown ids (menu ignores them).
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
  // Flips the persisted debug flag plus the runtime gate, then toasts the
  // new state so the user sees it took effect.
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
  // PC-IP dialog: validates the entry (empty clears the override) and
  // persists it; telemetry/camera pick it up without a restart.
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
              if (!ip.isEmpty() && !isValidIp(ip)) {
                Toast.makeText(context, R.string.pc_ip_invalid, Toast.LENGTH_LONG).show();
                return;
              }
              appSettings.setPcIp(ip);
            })
        .setNegativeButton(R.string.pc_ip_cancel, null)
        .show();
  }
  // Accepts anything InetAddress resolves (IPv4/IPv6/hostname). Static for
  // the unit test; the dialog rejects failures with a toast.
  static boolean isValidIp(String ip) {
    try {
      java.net.InetAddress.getByName(ip);
      return true;
    } catch (Exception e) {
      return false;
    }
  }
}
