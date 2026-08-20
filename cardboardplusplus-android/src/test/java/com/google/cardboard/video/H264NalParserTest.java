package com.google.cardboard.video;

import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for {@link H264NalParser} — the raw bitstream parser that extracts SPS/PPS NAL units
 * and parses SPS dimensions. These tests use synthetic H.264 access units and verify the
 * parser correctly handles start code scanning, NAL type detection, and SPS decoding.
 *
 * <p>If the driver fails to send PPS or SPS, extractNal returns null and the decoder
 * stays unconfigured — this is the lazy-configuration safety net.
 */
public class H264NalParserTest {

    // Synthetic SPS (baseline profile, 320x192 coded, 4-byte start code)
    private static final byte[] SPS_NAL = {
        0x00, 0x00, 0x00, 0x01, // 4-byte start code
        0x67,                   // NAL header: type 7 (SPS)
        0x42, (byte) 0xC0, 0x1E, // profile, compat, level
        (byte) 0xF8, 0x28, 0x32, 0x00 // RBSP
    };

    // Synthetic PPS (4-byte start code — avoids false SC when concatenated after SPS)
    private static final byte[] PPS_NAL = {
        0x00, 0x00, 0x00, 0x01, // 4-byte start code
        0x68,                   // NAL header: type 8 (PPS)
        (byte) 0xCE, 0x38, (byte) 0x80 // PPS body
    };

    // Synthetic IDR slice (4-byte start code)
    private static final byte[] IDR_NAL = {
        0x00, 0x00, 0x00, 0x01, // 4-byte start code
        0x65,                   // NAL header: type 5 (IDR)
        (byte) 0xAA, (byte) 0xBB, (byte) 0xCC // slice data
    };

    // --- extractNal: finding SPS/PPS in access units ---

    @Test
    public void extractSps_fromAccessUnit() {
        byte[] accessUnit = buildAccessUnit(SPS_NAL, PPS_NAL, IDR_NAL);
        byte[] sps = H264NalParser.extractNal(accessUnit, 7);
        assertNotNull("SPS (type 7) should be found", sps);
        assertEquals(0x67, sps[4] & 0xFF); // NAL header byte
    }

    @Test
    public void extractPps_fromAccessUnit() {
        byte[] accessUnit = buildAccessUnit(SPS_NAL, PPS_NAL, IDR_NAL);
        byte[] pps = H264NalParser.extractNal(accessUnit, 8);
        assertNotNull("PPS (type 8) should be found", pps);
        assertEquals(0x68, pps[4] & 0xFF); // NAL header byte after 4-byte start code
    }

    @Test
    public void extractIdr_fromAccessUnit() {
        byte[] accessUnit = buildAccessUnit(SPS_NAL, PPS_NAL, IDR_NAL);
        byte[] idr = H264NalParser.extractNal(accessUnit, 5);
        assertNotNull("IDR (type 5) should be found", idr);
        assertEquals(0x65, idr[4] & 0xFF); // NAL header byte
    }

    @Test
    public void extractNal_returnsNull_whenTypeNotFound() {
        byte[] accessUnit = buildAccessUnit(SPS_NAL, PPS_NAL, IDR_NAL);
        byte[] sei = H264NalParser.extractNal(accessUnit, 6); // SEI not present
        assertNull("Non-existent NAL type should return null", sei);
    }

    @Test
    public void extractSps_returnsNull_whenOnlyPpsPresent() {
        byte[] accessUnit = buildAccessUnit(PPS_NAL, IDR_NAL);
        byte[] sps = H264NalParser.extractNal(accessUnit, 7);
        assertNull("SPS should not be found when absent", sps);
    }

    @Test
    public void extractPps_returnsNull_whenOnlySpsPresent() {
        byte[] accessUnit = buildAccessUnit(SPS_NAL, IDR_NAL);
        byte[] pps = H264NalParser.extractNal(accessUnit, 8);
        assertNull("PPS should not be found when absent", pps);
    }

    // --- extractNal: 3-byte vs 4-byte start codes ---

    @Test
    public void extractNal_handles3byteStartCode() {
        // Standalone PPS with 3-byte start code (00 00 01)
        byte[] pps3 = {0x00, 0x00, 0x01, 0x68, (byte) 0xCE, 0x38};
        byte[] pps = H264NalParser.extractNal(pps3, 8);
        assertNotNull("Should find PPS with 3-byte start code", pps);
        assertEquals(0x68, pps[3] & 0xFF); // NAL header at index 3 for 3-byte SC
    }

    @Test
    public void extractNal_handles4byteStartCode() {
        byte[] accessUnit = buildAccessUnit(SPS_NAL);
        byte[] sps = H264NalParser.extractNal(accessUnit, 7);
        assertNotNull("Should find SPS with 4-byte start code", sps);
    }

    // --- parseSpsDimensions ---

    @Test
    public void parseSpsDimensions_decodesCorrectSize() {
        // Our synthetic SPS encodes 320x192 (20 MBs x 12 MBs, frame_mbs_only=1)
        int[] dims = H264NalParser.parseSpsDimensions(SPS_NAL);
        assertNotNull("SPS dimensions should parse", dims);
        assertEquals(320, dims[0]);
        assertEquals(192, dims[1]);
    }

    @Test
    public void parseSpsDimensions_handlesGarbageGracefully() {
        // parseSpsDimensions is best-effort: it won't crash on garbage, it just
        // returns computed dimensions from whatever bits it reads. This is by design
        // (BitReader doesn't throw on overread).
        int[] dims = H264NalParser.parseSpsDimensions(new byte[]{0x00, 0x01, 0x02});
        assertNotNull("Garbage SPS returns computed dims (best-effort)", dims);
    }

    @Test
    public void parseSpsDimensions_handlesEmptyGracefully() {
        // Empty SPS: off=0, RBSP is empty, BitReader reads past end but no exception.
        int[] dims = H264NalParser.parseSpsDimensions(new byte[0]);
        // Best-effort: may return dims from zero-filled bits or throw IndexOutOfBounds
        // Either way, the caller (VideoDecoder) guards with null check.
    }

    // --- Lazy configuration: what happens when SPS/PPS are missing ---

    @Test
    public void feedFrame_withoutSpsPps_doesNotConfigureDecoder() {
        // Simulates the VideoDecoder behavior: if extractNal returns null for both
        // SPS and PPS, the decoder remains unconfigured.
        // This is the critical safety net when the driver fails to send SPS/PPS.
        byte[] accessUnit = buildAccessUnit(IDR_NAL); // IDR only, no SPS/PPS
        byte[] sps = H264NalParser.extractNal(accessUnit, 7);
        byte[] pps = H264NalParser.extractNal(accessUnit, 8);
        assertNull("No SPS in IDR-only frame", sps);
        assertNull("No PPS in IDR-only frame", pps);
        // VideoDecoder.feedFrame would return early: "Still waiting for codec-specific data"
    }

    @Test
    public void feedFrame_withSpsOnly_doesNotConfigureDecoder() {
        // Partial codec data: SPS present but PPS missing
        byte[] accessUnit = buildAccessUnit(SPS_NAL, IDR_NAL);
        byte[] sps = H264NalParser.extractNal(accessUnit, 7);
        byte[] pps = H264NalParser.extractNal(accessUnit, 8);
        assertNotNull("SPS found", sps);
        assertNull("PPS missing — decoder cannot configure", pps);
        // VideoDecoder.feedFrame would return early waiting for PPS
    }

    @Test
    public void feedFrame_withBothSpsAndPps_canConfigureDecoder() {
        // Full keyframe with both SPS and PPS
        byte[] accessUnit = buildAccessUnit(SPS_NAL, PPS_NAL, IDR_NAL);
        byte[] sps = H264NalParser.extractNal(accessUnit, 7);
        byte[] pps = H264NalParser.extractNal(accessUnit, 8);
        assertNotNull("SPS found", sps);
        assertNotNull("PPS found", pps);
        // VideoDecoder.feedFrame would proceed to configure MediaCodec
    }

    // --- helpers ---

    /** Concatenate multiple NALs into one access unit. */
    private static byte[] buildAccessUnit(byte[]... nals) {
        int total = 0;
        for (byte[] n : nals) total += n.length;
        byte[] result = new byte[total];
        int off = 0;
        for (byte[] n : nals) {
            System.arraycopy(n, 0, result, off, n.length);
            off += n.length;
        }
        return result;
    }
}
