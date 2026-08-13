package com.google.cardboard.util;

import android.annotation.SuppressLint;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Size;

/** Camera-related static helpers. */
public final class CameraUtils {
  private CameraUtils() {}

  /** Returns the id of the first back-facing camera, or null if none is available. */
  @SuppressLint("MissingPermission")
  public static String findBackCameraId(CameraManager manager) throws CameraAccessException {
    String[] cameraIds = manager.getCameraIdList();
    for (String id : cameraIds) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(id);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
        return id;
      }
    }
    return null;
  }

  /**
   * Picks the first output size that is at least {@code minWidth}x{@code minHeight}, falling back
   * to the requested minimum size when nothing qualifies.
   */
  public static Size chooseOutputSize(
      StreamConfigurationMap map, int minWidth, int minHeight) {
    if (map != null) {
      Size[] outputSizes = map.getOutputSizes(android.graphics.SurfaceTexture.class);
      if (outputSizes != null) {
        for (Size size : outputSizes) {
          if (size.getWidth() >= minWidth && size.getHeight() >= minHeight) {
            return size;
          }
        }
      }
    }
    return new Size(minWidth, minHeight);
  }
}
