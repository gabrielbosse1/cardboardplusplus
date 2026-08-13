package com.google.cardboard.permissions;

import android.Manifest;
import android.content.pm.PackageManager;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import com.google.cardboard.core.AppConstants;

/** Centralises permission checks and requests used by the app. */
public class PermissionManager {
  private final AppCompatActivity activity;

  public PermissionManager(AppCompatActivity activity) {
    this.activity = activity;
  }

  public boolean isCameraGranted() {
    return ActivityCompat.checkSelfPermission(activity, Manifest.permission.CAMERA)
        == PackageManager.PERMISSION_GRANTED;
  }

  public void requestCamera() {
    ActivityCompat.requestPermissions(
        activity,
        new String[] {Manifest.permission.CAMERA},
        AppConstants.CAMERA_PERMISSIONS_REQUEST_CODE);
  }

  public boolean isReadExternalStorageGranted() {
    return ActivityCompat.checkSelfPermission(activity, Manifest.permission.READ_EXTERNAL_STORAGE)
        == PackageManager.PERMISSION_GRANTED;
  }

  public void requestReadExternalStorage() {
    final String[] permissions = new String[] {Manifest.permission.READ_EXTERNAL_STORAGE};
    ActivityCompat.requestPermissions(
        activity, permissions, AppConstants.PERMISSIONS_REQUEST_CODE);
  }

  public boolean shouldShowStorageRationale() {
    return ActivityCompat.shouldShowRequestPermissionRationale(
        activity, Manifest.permission.READ_EXTERNAL_STORAGE);
  }
}
