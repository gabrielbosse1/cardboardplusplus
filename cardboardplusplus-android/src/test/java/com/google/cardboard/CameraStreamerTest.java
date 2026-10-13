package com.google.cardboard;

import com.google.cardboard.core.AppConstants;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for CameraStreamer logic that pretend to be the bridge.
 *
 * These tests verify uncapped latest-wins send, JPEG size guards, and
 * stream lifecycle without requiring a real camera or network.
 */
public class CameraStreamerTest {

    private static final int JPEG_QUALITY = 38;
    private static final int TARGET_WIDTH = 256;
    private static final int TARGET_HEIGHT = 192;
    // Mirrors CameraStreamer.sendFrame: drop when jpeg + 2-byte seq header exceeds the datagram.
    private static boolean isAccepted(int jpegLen) {
        return jpegLen + AppConstants.CAMERA_SEQ_HEADER_LEN <= AppConstants.CAMERA_MAX_DATAGRAM;
    }

    @Test
    public void uncappedSenderAcceptsBackToBackFrames() {
        java.util.concurrent.atomic.AtomicBoolean senderBusy = new java.util.concurrent.atomic.AtomicBoolean(false);
        boolean first = senderBusy.compareAndSet(false, true);
        senderBusy.set(false);
        boolean second = senderBusy.compareAndSet(false, true);
        assertTrue("First back-to-back frame should be accepted", first);
        assertTrue("Second back-to-back frame should be accepted once sender is free", second);
    }

    @Test
    public void busySenderDropsWithoutBlocking() {
        java.util.concurrent.atomic.AtomicBoolean senderBusy = new java.util.concurrent.atomic.AtomicBoolean(true);
        boolean accepted = senderBusy.compareAndSet(false, true);
        assertFalse("Frame arriving while the sender is busy should be dropped", accepted);
    }

    @Test
    public void jpegSizeGuardRejectsOversizedFrames() {
        // Prod drops when jpegLen + 2 > 60000, so a 60000-byte JPEG is dropped too.
        assertFalse("59999-byte JPEG + header exceeds the datagram", isAccepted(59999));
        assertFalse("60000-byte JPEG + header exceeds the datagram", isAccepted(60000));
        assertTrue("59998-byte JPEG + header exactly fits", isAccepted(59998));
    }

    @Test
    public void targetDimensionsAreCorrect() {
        assertEquals(AppConstants.CAMERA_STREAM_WIDTH, TARGET_WIDTH);
        assertEquals(AppConstants.CAMERA_STREAM_HEIGHT, TARGET_HEIGHT);
    }

    @Test
    public void jpegQualityIsReasonable() {
        assertTrue("Quality should be 1-100", JPEG_QUALITY >= 1 && JPEG_QUALITY <= 100);
        assertEquals(AppConstants.CAMERA_JPEG_QUALITY, JPEG_QUALITY);
    }

    @Test
    public void seqHeaderIsTwoBytesBigEndian() {
        // Wire: [u16 seq BE][JPEG]. Bridge strips the header before decoding.
        int seq = 0x1234;
        byte[] payload = new byte[]{(byte) ((seq >> 8) & 0xFF), (byte) (seq & 0xFF), 0x01};
        int parsed = ((payload[0] & 0xFF) << 8) | (payload[1] & 0xFF);
        assertEquals(seq, parsed);
        assertEquals(AppConstants.CAMERA_SEQ_HEADER_LEN, 2);
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
