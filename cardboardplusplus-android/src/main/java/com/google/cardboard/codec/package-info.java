/**
 * Codec package: intentionally has no selector class.
 *
 * <p>The stream is AVC-only and streaming policy (resolution, bitrate, codec)
 * is owned by the bridge/driver (the phone announces its decode ceiling via
 * CARDBOARD_CAP). A codec-selection indirection with one wired codec was
 * removed; if a second codec ever ships, selection logic belongs here.
 */
package com.google.cardboard.codec;
