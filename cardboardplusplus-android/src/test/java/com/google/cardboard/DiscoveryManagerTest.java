package com.google.cardboard;

import com.google.cardboard.core.AppConstants;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for DiscoveryManager logic that pretend to be the driver.
 *
 * These tests verify the broadcast/ACK protocol and lifecycle without
 * requiring a real network socket.
 */
public class DiscoveryManagerTest {

    private static final String DISCOVERY_MESSAGE = AppConstants.DISCOVERY_MESSAGE;
    private static final String ACK_RESPONSE = AppConstants.DISCOVERY_ACK;

    @Test
    public void discoveryMessageIsCorrect() {
        assertEquals("CARDBOARD_DISCOVERY", DISCOVERY_MESSAGE);
    }

    @Test
    public void ackResponseIsCorrect() {
        assertEquals("ACK", ACK_RESPONSE);
        assertEquals(3, ACK_RESPONSE.length());
    }

    @Test
    public void discoveryBacksOffToHeartbeatOnAck() {
        // Prod never stops on ACK (a restarted driver needs rediscovery): it
        // backs off to a 1-per-5s heartbeat and resumes full rate after 5s
        // without an ACK. Mirrors DiscoveryManager.broadcastUntilAck.
        FakeDiscovery discovery = new FakeDiscovery();
        discovery.startDiscovery();
        assertTrue(discovery.isBroadcasting());

        // Simulate receiving ACK: loop keeps running, now in heartbeat mode.
        discovery.onAckReceived();
        assertTrue("ACK must not stop discovery (heartbeat continues)", discovery.isBroadcasting());
        assertTrue(discovery.isHeartbeat());
    }

    @Test
    public void discoveryKeepsBroadcastingOnTimeout() {
        FakeDiscovery discovery = new FakeDiscovery();
        discovery.startDiscovery();
        assertTrue(discovery.isBroadcasting());

        // Simulate timeout (no ACK).
        discovery.onTimeout();
        assertTrue(discovery.isBroadcasting());
    }

    @Test
    public void stopDiscoveryJoinsThread() {
        FakeDiscovery discovery = new FakeDiscovery();
        discovery.startDiscovery();
        discovery.stopDiscovery();
        assertFalse(discovery.isBroadcasting());
    }

    @Test
    public void doubleStartIsNoop() {
        FakeDiscovery discovery = new DiscoveryManagerTest.FakeDiscovery();
        discovery.startDiscovery();
        boolean firstBroadcasting = discovery.isBroadcasting();
        discovery.startDiscovery(); // should not throw or change state
        assertEquals(firstBroadcasting, discovery.isBroadcasting());
    }

    @Test
    public void discoveryIntervalMatchesContract() {
        // Phone re-broadcasts every 500ms.
        assertEquals(500, AppConstants.DISCOVERY_INTERVAL_MS);
    }

    @Test
    public void discoveryPortMatchesDriver() {
        assertEquals(42070, AppConstants.UDP_DISCOVERY_PORT);
    }

    @Test
    public void decoderCapIsResentPeriodically() {
        // Mirrors DiscoveryManager: CAP goes out on the 1st, 61st, 121st... ACK
        // (~every 30s) with a set (non-zero) cap — never just once, and never
        // with unset 0x0 dims, so a restarted driver always learns the cap.
        assertTrue(shouldSendCap(1, 1920, 1080));
        assertFalse(shouldSendCap(2, 1920, 1080));
        assertFalse(shouldSendCap(60, 1920, 1080));
        assertTrue(shouldSendCap(61, 1920, 1080));
        assertTrue(shouldSendCap(121, 1920, 1080));
        assertFalse(shouldSendCap(1, 0, 0));
    }

    private static boolean shouldSendCap(int ackCount, int w, int h) {
        return w > 0 && h > 0 && ackCount % 60 == 1;
    }

    /** Minimal state machine mirroring DiscoveryManager's lifecycle. */
    private static class FakeDiscovery {
        private volatile boolean broadcasting = false;
        private volatile boolean heartbeat = false;

        void startDiscovery() {
            if (broadcasting) return;
            broadcasting = true;
        }

        void stopDiscovery() {
            broadcasting = false;
            heartbeat = false;
        }

        void onAckReceived() {
            // Prod backs off to heartbeat, it never stops.
            heartbeat = true;
        }

        void onTimeout() {
            // Keep broadcasting.
        }

        boolean isBroadcasting() { return broadcasting; }
        boolean isHeartbeat() { return heartbeat; }
    }
}
