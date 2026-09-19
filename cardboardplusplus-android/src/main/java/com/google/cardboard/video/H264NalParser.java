package com.google.cardboard.video;

import java.util.Arrays;

/**
 * Static helpers for reading H.264 access units (annexb layout, 3- or 4-byte start codes).
 *
 * <p>Extracted from {@link VideoDecoder} so the decoder class only owns the MediaCodec pipeline and
 * this file owns the raw bitstream parsing, making both easier to reason about and to test.
 *
 * <p>Two capabilities are provided:
 *
 * <ul>
 *   <li>{@link #extractNal} - find a NAL unit of a given type (e.g. 7 = SPS, 8 = PPS) inside an
 *       access unit.
 *   <li>{@link #parseSpsDimensions} - parse the coded width/height out of an SPS NAL so the decoder
 *       can be (re)configured to the actual stream resolution.
 * </ul>
 */
final class H264NalParser {
  private H264NalParser() {}

  /**
   * Returns the first NAL unit of the given type (with its start code) or null.
   *
   * @param data    full access unit in annexb format
   * @param nalType H.264 nal_unit_type to look for (7 = SPS, 8 = PPS, 5 = IDR)
   */
  static byte[] extractNal(byte[] data, int nalType) {
    int n = data.length;
    int i = 0;
    // Need at least 3 bytes left for the shortest start code; the header byte
    // past the code is bounds-checked before reading.
    while (i + 2 < n) {
      int sc = startCodeLen(data, i);
      if (sc == 0) {
        i++;
        continue;
      }
      if (i + sc >= n) break; // start code at the very tail, no header byte
      int type = data[i + sc] & 0x1F;
      if (type == nalType) {
        int next = findNextStart(data, i + sc);
        int end = (next < 0) ? n : next;
        return Arrays.copyOfRange(data, i, end);
      }
      i += sc;
    }
    return null;
  }

  private static int startCodeLen(byte[] d, int i) {
    if (i + 3 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
      return 4;
    }
    if (i + 2 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      return 3;
    }
    return 0;
  }

  private static int findNextStart(byte[] d, int from) {
    // Same tail rule as extractNal: a start code can begin at the last 3 bytes.
    for (int i = Math.max(0, from); i + 2 < d.length; i++) {
      if (startCodeLen(d, i) != 0) return i;
    }
    return -1;
  }

  /**
   * Parse the visible width/height from an annexb SPS NAL (with start code).
   *
   * <p>The SPS bitstream is decoded far enough to reach {@code pic_width_in_mbs_minus1} /
   * {@code pic_height_in_map_units_minus1}, applies the frame_mbs_only_flag / chroma logic that
   * determines the coded dimensions, then subtracts the frame-crop rectangle
   * when {@code frame_cropping_flag} is set — so the result is the visible
   * size (e.g. 2880x1620), not the macroblock-coded size (e.g. 2880x1632).
   * Returns {@code null} if the NAL cannot be parsed.
   */
  static int[] parseSpsDimensions(byte[] sps) {
    try {
      int n = sps.length;
      // Skip start code (3 or 4 bytes).
      int off = (n > 4 && sps[2] == 1) ? 3 : (n > 5 && sps[3] == 1 ? 4 : 0);
      // Remove emulation prevention bytes (00 00 03 -> 00 00) for bit parsing.
      byte[] rbsp = new byte[n];
      int rlen = 0;
      for (int i = off; i < n; i++) {
        if (i + 2 < n && sps[i] == 0 && sps[i + 1] == 0 && sps[i + 2] == 3) {
          rbsp[rlen++] = sps[i];
          rbsp[rlen++] = sps[i + 1];
          i += 2; // skip the 03
        } else {
          rbsp[rlen++] = sps[i];
        }
      }
      BitReader br = new BitReader(rbsp, rlen);
      br.skip(8); // NAL header (profile/type already known)
      int profileIdc = br.read(8);
      br.skip(8); // constraint flags + reserved
      br.skip(8); // level_idc
      br.readUe(); // seq_parameter_set_id
      int chromaFormatIdc = 1; // default 4:2:0 when not present (baseline/main)
      if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122
          || profileIdc == 244 || profileIdc == 44 || profileIdc == 83
          || profileIdc == 86 || profileIdc == 118 || profileIdc == 128
          || profileIdc == 138 || profileIdc == 139 || profileIdc == 134
          || profileIdc == 135) {
        chromaFormatIdc = br.readUe(); // chroma_format_idc
        br.readUe(); // bit_depth_luma_minus8
        br.readUe(); // bit_depth_chroma_minus8
        br.skip(1); // qpprime
        int seqScaling = br.read(1);
        if (seqScaling == 1) {
          // scaling lists (skip 8x for 4x4 and 8x for 8x8, simplified)
          for (int i = 0; i < 8; i++) {
            int size = (i < 6) ? 16 : 64;
            int present = br.read(1);
            if (present == 1) {
              for (int j = 0; j < size; j++) br.readSe();
            }
          }
        }
      }
      br.readUe(); // log2_max_frame_num_minus4
      int pocType = br.readUe();
      if (pocType == 0) {
        br.readUe(); // log2_max_pic_order_cnt_lsb_minus4
      } else if (pocType == 1) {
        br.skip(1);
        br.readSe();
        br.readSe();
        int cycles = br.readUe();
        for (int i = 0; i < cycles; i++) br.readSe();
      }
      br.readUe(); // max_num_ref_frames
      br.skip(1); // gaps_in_frame_num_value_allowed_flag
      int wMbs = br.readUe() + 1;
      int hMbs = br.readUe() + 1;
      int frameMbsOnly = br.read(1);
      if (frameMbsOnly == 0) br.skip(1);
      br.skip(1); // direct_8x8_inference_flag
      int codedW = wMbs * 16;
      int codedH = hMbs * 16;
      if (frameMbsOnly == 0) codedH *= 2;
      // Visible size: subtract the frame-crop rectangle when present.
      if (br.read(1) == 1) { // frame_cropping_flag
        int cropLeft = br.readUe();
        int cropRight = br.readUe();
        int cropTop = br.readUe();
        int cropBottom = br.readUe();
        int cropUnitX;
        int cropUnitY;
        if (chromaFormatIdc == 0) {
          cropUnitX = 1;
          cropUnitY = 2 - frameMbsOnly;
        } else if (chromaFormatIdc == 1) {
          cropUnitX = 2;
          cropUnitY = 2 * (2 - frameMbsOnly);
        } else if (chromaFormatIdc == 2) {
          cropUnitX = 2;
          cropUnitY = (2 - frameMbsOnly);
        } else {
          cropUnitX = 1;
          cropUnitY = (2 - frameMbsOnly);
        }
        int visibleW = codedW - (cropLeft + cropRight) * cropUnitX;
        int visibleH = codedH - (cropTop + cropBottom) * cropUnitY;
        if (visibleW > 0 && visibleH > 0) {
          return new int[] { visibleW, visibleH };
        }
      }
      return new int[] { codedW, codedH };
    } catch (Exception e) {
      return null;
    }
  }

  /** Minimal big-endian bit reader over an RBSP buffer (no bounds checks beyond the buffer end). */
  private static final class BitReader {
    private final byte[] b;
    private final int len;
    private int pos; // bit position

    BitReader(byte[] b, int len) {
      this.b = b;
      this.len = len;
    }

    void skip(int n) {
      pos += n;
    }

    int read(int n) {
      int v = 0;
      for (int i = 0; i < n; i++) {
        int byteIdx = pos >> 3;
        if (byteIdx >= len) {
          pos++;
          continue;
        }
        int bit = (b[byteIdx] >> (7 - (pos & 7))) & 1;
        v = (v << 1) | bit;
        pos++;
      }
      return v;
    }

    int readUe() {
      int zeros = 0;
      while (read(1) == 0 && zeros < 32) zeros++;
      if (zeros == 0) return 0;
      return (1 << zeros) - 1 + read(zeros);
    }

    int readSe() {
      int k = readUe();
      if (k % 2 == 0) return -(k / 2);
      return (k + 1) / 2;
    }
  }
}