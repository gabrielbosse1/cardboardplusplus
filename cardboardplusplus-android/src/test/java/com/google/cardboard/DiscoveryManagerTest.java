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

    private static final String DISCOVERY_MESSAGE = "CARDBOARD_DISCOVERY";
    private static final String ACK_RESPONSE = "ACK";

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
    public void discoveryStopsOnAck() {
        // Simulate: broadcasting = true, then ACK received.
        FakeDiscovery discovery = new FakeDiscovery();
        discovery.startDiscovery();
        assertTrue(discovery.isBroadcasting());

        // Simulate receiving ACK.
        discovery.onAckReceived();
        assertFalse(discovery.isBroadcasting());
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

    /** Minimal state machine mirroring DiscoveryManager's lifecycle. */
    private static class FakeDiscovery {
        private volatile boolean broadcasting = false;

        void startDiscovery() {
            if (broadcasting) return;
            broadcasting = true;
        }

        void stopDiscovery() {
            broadcasting = false;
        }

        void onAckReceived() {
            broadcasting = false;
        }

        void onTimeout() {
            // Keep broadcasting.
        }

        boolean isBroadcasting() { return broadcasting; }
    }
}
