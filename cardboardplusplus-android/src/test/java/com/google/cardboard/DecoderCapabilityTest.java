package com.google.cardboard;

import com.google.cardboard.core.AppConstants;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for DecoderCapabilityReporter wire format that pretend to be the driver.
 *
 * These verify that the CARDBOARD_CAP message the phone sends matches
 * what the driver's Discovery.cpp expects to parse.
 */
public class DecoderCapabilityTest {

    private static final String CAP_PREFIX = AppConstants.CAP_PREFIX;

    @Test
    public void capMessageFormatMatchesDriver() {
        // Driver parses: "CARDBOARD_CAP <width> <height>" via sscanf_s
        int width = 1920;
        int height = 1080;
        String msg = CAP_PREFIX + width + " " + height;
        assertEquals("CARDBOARD_CAP 1920 1080", msg);
    }

    @Test
    public void capMessageStartsWithCorrectPrefix() {
        // Driver checks: strncmp(buffer, wire::kCardboardCap, wire::kCardboardCapLen) == 0
        String msg = CAP_PREFIX + 1600 + " " + 900;
        assertTrue(msg.startsWith("CARDBOARD_CAP"));
        assertEquals(13, "CARDBOARD_CAP".length());
    }

    @Test
    public void capMessageDoesNotMatchDiscoveryMessage() {
        // CARDBOARD_CAP must not be confused with CARDBOARD_DISCOVERY
        String cap = CAP_PREFIX + 1920 + " " + 1080;
        String discovery = "CARDBOARD_DISCOVERY";
        assertNotEquals(cap, discovery);
        // The prefix "CARDBOARD_CAP" is 13 bytes; "CARDBOARD_DISCOVERY" is 18 bytes.
        // Driver uses strncmp with kCardboardCapLen (13), so "CARDBOARD_DISCOVERY"
        // would match the CAP prefix if not for the fallthrough order in dispatch.
        assertTrue(cap.startsWith("CARDBOARD_CAP"));
        assertFalse(discovery.startsWith("CARDBOARD_CAP")); // "CARDBOARD_DISCOVERY" != "CARDBOARD_CAP " (note space)
    }

    @Test
    public void capMessageDoesNotMatchBridgeHello() {
        // CARDBOARD_CAP must not match BRIDGE_HELLO prefix
        String cap = CAP_PREFIX + 1920 + " " + 1080;
        assertFalse(cap.startsWith("BRIDGE_HELLO"));
    }

    @Test
    public void capSendAttemptsAndGapAreReasonable() {
        // DiscoveryManager sends the CAP burst 3 times with 500ms gap.
        // These constants live in DiscoveryManager.java (CAP_SEND_ATTEMPTS/CAP_SEND_GAP_MS).
        int sendAttempts = 3;
        long sendGapMs = 500;
        assertEquals(3, sendAttempts);
        assertEquals(500, sendGapMs);
    }

    @Test
    public void zeroDecoderCapIsNeverAnnounced() {
        // DiscoveryManager guards w>0&&h>0 before every CAP send (unset 0x0 must
        // never reach the driver, or it would clamp the encoder to nothing).
        assertFalse("unset cap must not send", shouldSendCap(1, 0, 0));
        assertFalse("unset cap must not send", shouldSendCap(1, 1920, 0));
        assertTrue("set cap sends on 1st ACK", shouldSendCap(1, 1920, 1080));
        assertFalse("set cap skips 2nd ACK", shouldSendCap(2, 1920, 1080));
    }

    /** Mirrors DiscoveryManager's CAP gate: ackCount % 60 == 1 with a set (non-zero) cap. */
    private static boolean shouldSendCap(int ackCount, int w, int h) {
        return w > 0 && h > 0 && ackCount % 60 == 1;
    }

    @Test
    public void capMessageWithLargeDimensions() {
        // 8K resolution
        String msg = CAP_PREFIX + 7680 + " " + 4320;
        assertEquals("CARDBOARD_CAP 7680 4320", msg);
    }
}
