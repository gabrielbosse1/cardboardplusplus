package com.google.cardboard.camera;

import android.annotation.SuppressLint;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Size;

/**
 * Camera-related static helpers used by {@link CameraController}.
 *
 * <p>Lives next to {@link CameraController} in the {@code camera} package because it exists only to
 * serve the Camera2 lifecycle (device selection + output-size negotiation), not general purpose
 * camera code.
 */
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
   * Picks the smallest output size that is at least {@code minWidth}x{@code minHeight},
   * falling back to the requested minimum size when nothing qualifies.
   * Smallest-qualifying keeps the sensor in a fast readout mode (high fps);
   * the old first-match picked the largest (e.g. 2304x1728) and capped the
   * streamer at ~8fps.
   */
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

  /**
   * Same smallest-qualifying pick but for {@link android.media.ImageReader}
   * (YUV_420_888) outputs, whose supported sizes differ from SurfaceTexture's.
   * The streamer only needs 256x192, so request small directly instead of
   * capturing full-res and downscaling every frame in Java.
   */
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