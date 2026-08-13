package com.google.cardboard.streaming;

import com.google.cardboard.settings.AppSettings;

/**
 * Planned: streams the phone's camera feed back to the PC.
 *
 * <p>This is a structural placeholder wired for future work. It is intentionally not started by
 * {@code VrActivity} yet, so current behaviour is unchanged. Once a native encoder and a PC
 * receiver exist, {@code start()} will bind to the {@code CameraController} output and push encoded
 * frames to the PC using the configured codec / frame rate / bitrate.
 */
public class CameraStreamer {
  private static final int PC_PORT = 42071;

  private final AppSettings settings;
  private boolean streaming = false;

  public CameraStreamer(AppSettings settings) {
    this.settings = settings;
  }

  public void start() {
    if (streaming) {
      return;
    }
    // TODO: bind to CameraController output and push encoded frames to the PC at PC_PORT using
    // settings (codec / frame rate / bitrate) once a native encoder + PC receiver exist.
    streaming = true;
  }

  public void stop() {
    streaming = false;
  }

  public boolean isStreaming() {
    return streaming;
  }
}
