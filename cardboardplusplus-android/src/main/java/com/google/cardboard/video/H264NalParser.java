package com.google.cardboard.video;
import java.util.Arrays;
// Annex-B NAL splitter and SPS dimension reader in video/; VideoDecoder uses it to configure MediaCodec.
final class H264NalParser {
  private H264NalParser() {}
  // Returns the first NAL of the given type including its start code; called from VideoDecoder.feedFrame.
  static byte[] extractNal(byte[] data, int nalType) {
    int n = data.length;
    int i = 0;
    while (i + 2 < n) {
      int sc = startCodeLen(data, i);
      if (sc == 0) {
        i++;
        continue;
      }
      if (i + sc >= n) break;
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
  // Measures a 3- or 4-byte start code at the offset; called from extract and findNextStart scans.
  private static int startCodeLen(byte[] d, int i) {
    if (i + 3 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
      return 4;
    }
    if (i + 2 < d.length && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      return 3;
    }
    return 0;
  }
  // Finds the next start code at or after the offset; called from extractNal to bound the NAL slice.
  private static int findNextStart(byte[] d, int from) {
    for (int i = Math.max(0, from); i + 2 < d.length; i++) {
      if (startCodeLen(d, i) != 0) return i;
    }
    return -1;
  }
  // Decodes visible width and height from SPS RBSP; called from VideoDecoder.feedFrame on keyframes.
  static int[] parseSpsDimensions(byte[] sps) {
    try {
      int n = sps.length;
      int off = (n > 4 && sps[2] == 1) ? 3 : (n > 5 && sps[3] == 1 ? 4 : 0);
      byte[] rbsp = new byte[n];
      int rlen = 0;
      for (int i = off; i < n; i++) {
        if (i + 2 < n && sps[i] == 0 && sps[i + 1] == 0 && sps[i + 2] == 3) {
          rbsp[rlen++] = sps[i];
          rbsp[rlen++] = sps[i + 1];
          i += 2;
        } else {
          rbsp[rlen++] = sps[i];
        }
      }
      BitReader br = new BitReader(rbsp, rlen);
      br.skip(8);
      int profileIdc = br.read(8);
      br.skip(8);
      br.skip(8);
      br.readUe();
      int chromaFormatIdc = 1;
      if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122
          || profileIdc == 244 || profileIdc == 44 || profileIdc == 83
          || profileIdc == 86 || profileIdc == 118 || profileIdc == 128
          || profileIdc == 138 || profileIdc == 139 || profileIdc == 134
          || profileIdc == 135) {
        chromaFormatIdc = br.readUe();
        br.readUe();
        br.readUe();
        br.skip(1);
        int seqScaling = br.read(1);
        if (seqScaling == 1) {
          for (int i = 0; i < 8; i++) {
            int size = (i < 6) ? 16 : 64;
            int present = br.read(1);
            if (present == 1) {
              for (int j = 0; j < size; j++) br.readSe();
            }
          }
        }
      }
      br.readUe();
      int pocType = br.readUe();
      if (pocType == 0) {
        br.readUe();
      } else if (pocType == 1) {
        br.skip(1);
        br.readSe();
        br.readSe();
        int cycles = br.readUe();
        for (int i = 0; i < cycles; i++) br.readSe();
      }
      br.readUe();
      br.skip(1);
      int wMbs = br.readUe() + 1;
      int hMbs = br.readUe() + 1;
      int frameMbsOnly = br.read(1);
      if (frameMbsOnly == 0) br.skip(1);
      br.skip(1);
      int codedW = wMbs * 16;
      int codedH = hMbs * 16;
      if (frameMbsOnly == 0) codedH *= 2;
      if (br.read(1) == 1) {
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
  // Bit-level SPS cursor; VideoDecoder never touches it directly, only via parseSpsDimensions.
  private static final class BitReader {
    private final byte[] b;
    private final int len;
    private int pos;
    // Stores the RBSP window; called from parseSpsDimensions after emulation-prevention removal.
    BitReader(byte[] b, int len) {
      this.b = b;
      this.len = len;
    }
    // Advances the bit position; called throughout SPS field parsing.
    void skip(int n) {
      pos += n;
    }
    // Reads n bits big-endian; called throughout SPS field parsing.
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
    // Reads an unsigned Exp-Golomb code; called for SPS width, height, and crop fields.
    int readUe() {
      int zeros = 0;
      while (read(1) == 0 && zeros < 32) zeros++;
      if (zeros == 0) return 0;
      return (1 << zeros) - 1 + read(zeros);
    }
    // Reads a signed Exp-Golomb code; called for SPS scaling-list and POC fields.
    int readSe() {
      int k = readUe();
      if (k % 2 == 0) return -(k / 2);
      return (k + 1) / 2;
    }
  }
}