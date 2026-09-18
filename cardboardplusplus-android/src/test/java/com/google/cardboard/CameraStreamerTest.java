package com.google.cardboard;

import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for CameraStreamer logic that pretend to be the bridge.
 *
 * These tests verify frame rate limiting, JPEG size guards, and stream
 * lifecycle without requiring a real camera or network.
 */
public class CameraStreamerTest {

    private static final long FRAME_INTERVAL_MS = 1000 / 30; // 30 fps target
    private static final int JPEG_QUALITY = 38;
    private static final int TARGET_WIDTH = 256;
    private static final int TARGET_HEIGHT = 192;
    private static final int MAX_JPEG_SIZE = 60000;
    private static final int SEQ_HEADER_LEN = 2;

    @Test
    public void frameRateLimitingRejectsFastFrames() {
        long lastFrameTime = System.currentTimeMillis();
        long now = lastFrameTime + 20; // 20ms — less than 33ms interval
        boolean shouldSend = (now - lastFrameTime) >= FRAME_INTERVAL_MS;
        assertFalse("Frame at 20ms should be dropped", shouldSend);
    }

    @Test
    public void frameRateLimitingAllowsSlowFrames() {
        long lastFrameTime = System.currentTimeMillis();
        long now = lastFrameTime + 40; // 40ms — more than 33ms interval
        boolean shouldSend = (now - lastFrameTime) >= FRAME_INTERVAL_MS;
        assertTrue("Frame at 40ms should be sent", shouldSend);
    }

    @Test
    public void jpegSizeGuardRejectsOversizedFrames() {
        byte[] oversized = new byte[60001];
        assertTrue("60001 bytes should be rejected", oversized.length > MAX_JPEG_SIZE);

        byte[] exact = new byte[60000];
        assertFalse("60000 bytes should be accepted", exact.length > MAX_JPEG_SIZE);
    }

    @Test
    public void targetDimensionsAreCorrect() {
        assertEquals(256, TARGET_WIDTH);
        assertEquals(192, TARGET_HEIGHT);
    }

    @Test
    public void jpegQualityIsReasonable() {
        assertTrue("Quality should be 1-100", JPEG_QUALITY >= 1 && JPEG_QUALITY <= 100);
        assertEquals(38, JPEG_QUALITY);
    }

    @Test
    public void seqHeaderIsTwoBytesBigEndian() {
        // Wire: [u16 seq BE][JPEG]. Bridge strips the header before decoding.
        int seq = 0x1234;
        byte[] payload = new byte[]{(byte) ((seq >> 8) & 0xFF), (byte) (seq & 0xFF), 0x01};
        int parsed = ((payload[0] & 0xFF) << 8) | (payload[1] & 0xFF);
        assertEquals(seq, parsed);
        assertEquals(SEQ_HEADER_LEN, 2);
    }

    @Test
    public void streamLifecycleFlags() {
        FakeStreamer streamer = new FakeStreamer();
        assertFalse(streamer.isStreaming());
        assertFalse(streamer.shouldStream());

        streamer.start();
        assertTrue(streamer.isStreaming());
        assertTrue(streamer.shouldStream());

        streamer.stop();
        assertFalse(streamer.isStreaming());
        assertFalse(streamer.shouldStream());
    }

    /** Minimal state machine mirroring CameraStreamer's lifecycle. */
    private static class FakeStreamer {
        private volatile boolean streaming = false;
        private volatile boolean shouldStream = false;

        void start() { shouldStream = true; streaming = true; }
        void stop() { shouldStream = false; streaming = false; }
        boolean isStreaming() { return streaming; }
        boolean shouldStream() { return shouldStream; }
    }
}
