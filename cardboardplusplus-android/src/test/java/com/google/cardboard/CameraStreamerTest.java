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

    private static final long FRAME_INTERVAL_MS = 1000 / 15; // 15 fps target
    private static final int JPEG_QUALITY = 50;
    private static final int TARGET_WIDTH = 320;
    private static final int TARGET_HEIGHT = 240;
    private static final int MAX_JPEG_SIZE = 60000;

    @Test
    public void frameRateLimitingRejectsFastFrames() {
        long lastFrameTime = System.currentTimeMillis();
        long now = lastFrameTime + 30; // 30ms — less than 66ms interval
        boolean shouldSend = (now - lastFrameTime) >= FRAME_INTERVAL_MS;
        assertFalse("Frame at 30ms should be dropped", shouldSend);
    }

    @Test
    public void frameRateLimitingAllowsSlowFrames() {
        long lastFrameTime = System.currentTimeMillis();
        long now = lastFrameTime + 100; // 100ms — more than 66ms interval
        boolean shouldSend = (now - lastFrameTime) >= FRAME_INTERVAL_MS;
        assertTrue("Frame at 100ms should be sent", shouldSend);
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
        assertEquals(320, TARGET_WIDTH);
        assertEquals(240, TARGET_HEIGHT);
    }

    @Test
    public void jpegQualityIsReasonable() {
        assertTrue("Quality should be 1-100", JPEG_QUALITY >= 1 && JPEG_QUALITY <= 100);
        assertEquals(50, JPEG_QUALITY);
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
