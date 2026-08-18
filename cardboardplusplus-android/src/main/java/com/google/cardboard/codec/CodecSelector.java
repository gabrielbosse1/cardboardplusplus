package com.google.cardboard.codec;

import com.google.cardboard.settings.AppSettings;

/**
 * Resolves which codec the decoder should use.
 *
 * <p>Currently only H264 is wired into the native decoder, so this simply forwards the user's
 * configured codec. Kept as a separate indirection because {@link AppSettings} is a persisted
 * preference store and this is where future real selection logic (capability probing, encoder-side
 * negotiation) would live.
 */
public class CodecSelector {
  private final AppSettings settings;

  public CodecSelector(AppSettings settings) {
    this.settings = settings;
  }

  public VideoCodec select() {
    return settings.getCodec();
  }
}
