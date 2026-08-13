package com.google.cardboard.codec;

import com.google.cardboard.settings.AppSettings;

/**
 * Resolves which codec the decoder should use. Currently only H264 is wired into the native
 * decoder, but this selector is the single place to extend when additional encoder/codec support
 * is added on the PC side.
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
