package com.google.cardboard;

import com.google.cardboard.core.AppConstants;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests that pretend to be the bridge and driver — verifying the phone's
 * wire protocol constants match the locked contract in CardboardWire.h.
 */
public class WireContractTest {

    @Test
    public void discoveryPortMatchesDriver() {
        // Phone broadcasts discovery on this port; driver listens here.
        assertEquals(42070, AppConstants.UDP_DISCOVERY_PORT);
    }

    @Test
    public void videoPortMatchesDriver() {
        // Driver sends H.264 video to the phone on this port.
        assertEquals(42069, AppConstants.VIDEO_PORT);
    }

    @Test
    public void discoveryIntervalMatchesBridgeExpectation() {
        // Phone re-broadcasts every 500ms until ACK.
        assertEquals(500, AppConstants.DISCOVERY_INTERVAL_MS);
    }

    @Test
    public void cameraStreamerPortMatchesBridge() {
        // CameraStreamer sends JPEG frames to bridge on this port.
        assertEquals(42072, AppConstants.CAMERA_PORT);
    }

    @Test
    public void telemetryPortMatchesBridge() {
        // Phone sends gyro/hand/ping telemetry to bridge on this port.
        assertEquals(42071, AppConstants.TELEMETRY_PORT);
    }

    @Test
    public void discoveryMessageIsExactlyCardboardDiscovery() {
        // The phone broadcasts DISCOVERY_MESSAGE — not the CAP prefix.
        assertEquals("CARDBOARD_DISCOVERY", AppConstants.DISCOVERY_MESSAGE);
        assertNotEquals(AppConstants.CAP_PREFIX.trim(), AppConstants.DISCOVERY_MESSAGE);
    }

    @Test
    public void ackResponseIsExactlyAck() {
        // The driver replies with just DISCOVERY_ACK (3 bytes, no newline).
        assertEquals(3, AppConstants.DISCOVERY_ACK.length());
        assertEquals("ACK", AppConstants.DISCOVERY_ACK);
    }

    @Test
    public void phoneHelloFormatMatchesBridgeParser() {
        // Phone sends PHONE_HELLO on first contact.
        assertTrue(AppConstants.PHONE_HELLO.startsWith("CARDBOARD_PHONE_HELLO"));
        assertTrue(AppConstants.PHONE_HELLO.endsWith("v1"));
    }

    @Test
    public void defaultCameraDimensionsAreSane() {
        assertTrue(AppConstants.DEFAULT_CAMERA_WIDTH >= 320);
        assertTrue(AppConstants.DEFAULT_CAMERA_HEIGHT >= 240);
    }
}
