package com.google.cardboard.camera;
import android.annotation.SuppressLint;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Size;
// Camera2 selection helpers; owns back-camera lookup and output-size choice for CameraController.
public final class CameraUtils {
  private CameraUtils() {}
  // Returns the back-facing camera id or null; called from CameraController.openCamera on the UI thread.
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
  // Picks the smallest SurfaceTexture size meeting the minimums; called from CameraController.openCamera for preview.
  public static Size chooseOutputSize(
      StreamConfigurationMap map, int minWidth, int minHeight) {
    Size best = null;
    if (map != null) {
      Size[] outputSizes = map.getOutputSizes(android.graphics.SurfaceTexture.class);
      if (outputSizes != null) {
        for (Size size : outputSizes) {
          if (size.getWidth() >= minWidth && size.getHeight() >= minHeight) {
            if (best == null
                || (long) size.getWidth() * size.getHeight()
                    < (long) best.getWidth() * best.getHeight()) {
              best = size;
            }
          }
        }
      }
    }
    return best != null ? best : new Size(minWidth, minHeight);
  }
  // Picks the smallest YUV_420_888 size meeting the minimums; called from CameraController.openCamera for streaming.
  public static Size chooseYuvOutputSize(
      StreamConfigurationMap map, int minWidth, int minHeight) {
    Size best = null;
    if (map != null) {
      Size[] outputSizes = map.getOutputSizes(android.graphics.ImageFormat.YUV_420_888);
      if (outputSizes != null) {
        for (Size size : outputSizes) {
          if (size.getWidth() >= minWidth && size.getHeight() >= minHeight) {
            if (best == null
                || (long) size.getWidth() * size.getHeight()
                    < (long) best.getWidth() * best.getHeight()) {
              best = size;
            }
          }
        }
      }
    }
    return best != null ? best : new Size(minWidth, minHeight);
  }
}
