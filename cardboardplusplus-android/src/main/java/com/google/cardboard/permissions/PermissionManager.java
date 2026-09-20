package com.google.cardboard.permissions;
import android.Manifest;
import android.content.pm.PackageManager;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import com.google.cardboard.core.AppConstants;
// Permission gate in permissions/; VrActivity consults it before starting camera and storage flows.
// Constructor stores the host activity; called from VrActivity.onCreate on the UI thread.
public class PermissionManager {
  private final AppCompatActivity activity;
  // Stores the host activity; called from VrActivity.onCreate on the UI thread.
  public PermissionManager(AppCompatActivity activity) {
    this.activity = activity;
  }
  // Reports camera grant state; called from VrActivity resume gate and VrRenderer setup.
  public boolean isCameraGranted() {
    return ActivityCompat.checkSelfPermission(activity, Manifest.permission.CAMERA)
        == PackageManager.PERMISSION_GRANTED;
  }
  // Asks the system for camera access; called from VrActivity's resume gate on the UI thread.
  public void requestCamera() {
    ActivityCompat.requestPermissions(
        activity,
        new String[] {Manifest.permission.CAMERA},
        AppConstants.CAMERA_PERMISSIONS_REQUEST_CODE);
  }
  // Reports storage grant state; called from VrActivity's resume gate on pre-Q devices.
  public boolean isReadExternalStorageGranted() {
    return ActivityCompat.checkSelfPermission(activity, Manifest.permission.READ_EXTERNAL_STORAGE)
        == PackageManager.PERMISSION_GRANTED;
  }
  // Asks the system for storage access; called from VrActivity's resume gate on the UI thread.
  public void requestReadExternalStorage() {
    final String[] permissions = new String[] {Manifest.permission.READ_EXTERNAL_STORAGE};
    ActivityCompat.requestPermissions(
        activity, permissions, AppConstants.PERMISSIONS_REQUEST_CODE);
  }
  // Reports whether storage rationale is showable; called from VrActivity's permission-result path.
  public boolean shouldShowStorageRationale() {
    return ActivityCompat.shouldShowRequestPermissionRationale(
        activity, Manifest.permission.READ_EXTERNAL_STORAGE);
  }
  // Reports whether camera rationale is showable; called from VrActivity's permission-result path.
  public boolean shouldShowCameraRationale() {
    return ActivityCompat.shouldShowRequestPermissionRationale(
        activity, Manifest.permission.CAMERA);
  }
}
