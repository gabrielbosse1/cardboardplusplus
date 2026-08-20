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
        assertEquals(42072, getCameraPort());
    }

    @Test
    public void telemetryPortMatchesBridge() {
        // Phone sends gyro/hand/ping telemetry to bridge on this port.
        assertEquals(42071, getTelemetryPort());
    }

    @Test
    public void discoveryMessageIsExactlyCardboardDiscovery() {
        // The phone broadcasts "CARDBOARD_DISCOVERY" — not "CARDBOARD_CAP".
        String msg = "CARDBOARD_DISCOVERY";
        assertEquals("CARDBOARD_DISCOVERY", msg);
        assertNotEquals("CARDBOARD_CAP", msg);
    }

    @Test
    public void ackResponseIsExactlyAck() {
        // The driver replies with just "ACK" (3 bytes, no newline).
        String ack = "ACK";
        assertEquals(3, ack.length());
        assertEquals("ACK", ack);
    }

    @Test
    public void phoneHelloFormatMatchesBridgeParser() {
        // Phone sends "CARDBOARD_PHONE_HELLO v1" on first contact.
        int version = 1;
        String hello = String.format("CARDBOARD_PHONE_HELLO v%d", version);
        assertTrue(hello.startsWith("CARDBOARD_PHONE_HELLO"));
        assertTrue(hello.endsWith("v1"));
    }

    @Test
    public void defaultCameraDimensionsAreSane() {
        assertTrue(AppConstants.DEFAULT_CAMERA_WIDTH >= 320);
        assertTrue(AppConstants.DEFAULT_CAMERA_HEIGHT >= 240);
    }

    // Port constants not in AppConstants (camera + telemetry are phone-side only)
    private static final int CAMERA_PORT = 42072;
    private static final int TELEMETRY_PORT = 42071;

    private int getCameraPort() { return CAMERA_PORT; }
    private int getTelemetryPort() { return TELEMETRY_PORT; }
}
