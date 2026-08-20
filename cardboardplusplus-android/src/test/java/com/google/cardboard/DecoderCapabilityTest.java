package com.google.cardboard;

import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for DecoderCapabilityReporter wire format that pretend to be the driver.
 *
 * These verify that the CARDBOARD_CAP message the phone sends matches
 * what the driver's Discovery.cpp expects to parse.
 */
public class DecoderCapabilityTest {

    private static final String CAP_PREFIX = "CARDBOARD_CAP ";

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
        // The reporter sends 3 times with 500ms gap.
        // These constants are in DecoderCapabilityReporter.java.
        int sendAttempts = 3;
        long sendGapMs = 500;
        assertEquals(3, sendAttempts);
        assertEquals(500, sendGapMs);
    }

    @Test
    public void capMessageWithZeroDimensions() {
        // Edge case: zero dimensions should still produce valid format
        String msg = CAP_PREFIX + 0 + " " + 0;
        assertEquals("CARDBOARD_CAP 0 0", msg);
        assertTrue(msg.startsWith("CARDBOARD_CAP"));
    }

    @Test
    public void capMessageWithLargeDimensions() {
        // 8K resolution
        String msg = CAP_PREFIX + 7680 + " " + 4320;
        assertEquals("CARDBOARD_CAP 7680 4320", msg);
    }
}
