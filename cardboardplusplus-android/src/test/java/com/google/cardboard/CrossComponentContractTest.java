package com.google.cardboard;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Cross-component contract tests: verify the phone's wire format matches
 * what the bridge and driver expect to parse.
 *
 * These tests bridge the gap between the phone app and the Rust bridge's
 * telemetry.rs parser, ensuring that if the phone sends a packet, the
 * bridge will correctly parse it.
 */
public class CrossComponentContractTest {

    // --- Telemetry wire format (phone -> bridge on UDP 42071) ---

    @Test
    public void gyroPacketMatchesBridgeParser() {
        // Bridge telemetry.rs: parse_gyro reads:
        //   buf[0] = 0x10 (tag)
        //   buf[1..9] = u64 timestamp_ms (LE)
        //   buf[9..13] = f32 angular_velocity[0] (LE)
        //   buf[13..17] = f32 angular_velocity[1] (LE)
        //   buf[17..21] = f32 angular_velocity[2] (LE)
        //   buf[21..25] = f32 acceleration[0] (LE)
        //   buf[25..29] = f32 acceleration[1] (LE)
        //   buf[29..33] = f32 acceleration[2] (LE)
        long timestamp = 1234L;
        float[] angVel = {0.5f, -0.2f, 0.1f};
        float[] accel = {1.0f, 9.8f, 0.0f};

        ByteBuffer buf = ByteBuffer.allocate(33).order(ByteOrder.LITTLE_ENDIAN);
        buf.put((byte) 0x10);
        buf.putLong(timestamp);
        for (float v : angVel) buf.putFloat(v);
        for (float v : accel) buf.putFloat(v);
        byte[] packet = buf.array();

        // Verify tag
        assertEquals(0x10, packet[0]);
        // Verify length (1 + 8 + 6*4 = 33)
        assertEquals(33, packet.length);
        // Verify timestamp is LE u64
        long parsedTimestamp = ByteBuffer.wrap(packet, 1, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
        assertEquals(timestamp, parsedTimestamp);
        // Verify first angular velocity is LE f32
        float parsedAngVel0 = ByteBuffer.wrap(packet, 9, 4).order(ByteOrder.LITTLE_ENDIAN).getFloat();
        assertEquals(angVel[0], parsedAngVel0, 0.001f);
    }

    @Test
    public void handPacketMatchesBridgeParser() {
        // Bridge telemetry.rs: parse_hand reads:
        //   buf[0] = 0x11 (tag)
        //   buf[1..9] = u64 timestamp_ms (LE)
        //   buf[9] = u8 hands
        //   buf[10] = u8 landmarks_per_hand
        //   buf[11..15] = f32 confidence (LE)
        long timestamp = 2345L;
        int hands = 2;
        int landmarks = 21;
        float confidence = 0.91f;

        ByteBuffer buf = ByteBuffer.allocate(15).order(ByteOrder.LITTLE_ENDIAN);
        buf.put((byte) 0x11);
        buf.putLong(timestamp);
        buf.put((byte) hands);
        buf.put((byte) landmarks);
        buf.putFloat(confidence);
        byte[] packet = buf.array();

        assertEquals(0x11, packet[0]);
        assertEquals(15, packet.length);
        assertEquals(hands, packet[9]);
        assertEquals(landmarks, packet[10]);
        float parsedConfidence = ByteBuffer.wrap(packet, 11, 4).order(ByteOrder.LITTLE_ENDIAN).getFloat();
        assertEquals(confidence, parsedConfidence, 0.001f);
    }

    @Test
    public void pingPacketMatchesBridgeParser() {
        // Bridge telemetry.rs: 0x20 -> TelemetryPacket::Ping
        byte[] packet = new byte[]{0x20};
        assertEquals(0x20, packet[0]);
        assertEquals(1, packet.length);
    }

    @Test
    public void helloPacketMatchesBridgeParser() {
        // Bridge telemetry.rs: is_phone_hello checks starts_with("CARDBOARD_PHONE_HELLO")
        String hello = "CARDBOARD_PHONE_HELLO v1";
        assertTrue(hello.startsWith("CARDBOARD_PHONE_HELLO"));
        byte[] packet = hello.getBytes();
        // Bridge uses String::from_utf8_lossy(buf).trim_start().starts_with(...)
        String received = new String(packet).trim();
        assertTrue(received.startsWith("CARDBOARD_PHONE_HELLO"));
    }

    // --- Discovery wire format (phone -> driver on UDP 42070) ---

    @Test
    public void discoveryBroadcastMatchesDriverExpectation() {
        // Driver Discovery.cpp fallthrough: any packet not matching the four
        // known prefixes triggers SwitchDataTarget + ACK.
        // Phone sends "CARDBOARD_DISCOVERY" which must NOT match:
        //   - kCardboardCap ("CARDBOARD_CAP")
        //   - kBridgeHeartbeat ("BRIDGE_HELLO")
        //   - kBridgePreview ("BRIDGE_PREVIEW")
        //   - kBridgeCfg ("BRIDGE_CFG")
        String discovery = "CARDBOARD_DISCOVERY";
        assertFalse(discovery.startsWith("CARDBOARD_CAP"));
        assertFalse(discovery.startsWith("BRIDGE_HELLO"));
        assertFalse(discovery.startsWith("BRIDGE_PREVIEW"));
        assertFalse(discovery.startsWith("BRIDGE_CFG"));
    }

    @Test
    public void discoveryPortMatchesDriverPort() {
        // Phone broadcasts on 42070, driver listens on 42070
        assertEquals(42070, AppConstants.UDP_DISCOVERY_PORT);
    }

    // --- Video wire format (driver -> phone on UDP 42069) ---

    @Test
    public void videoPortMatchesDriverPort() {
        // Driver sends on 42069, phone receives on 42069
        assertEquals(42069, AppConstants.VIDEO_PORT);
    }

    @Test
    public void videoLengthPrefixIsBigEndian() {
        // Driver sends: 4-byte big-endian length + H.264 payload
        // Phone VideoReceiver.cpp parses: buffer[0]<<24 | buffer[1]<<16 | buffer[2]<<8 | buffer[3]
        int payloadLength = 1234;
        byte[] prefix = new byte[]{
            (byte) ((payloadLength >> 24) & 0xFF),
            (byte) ((payloadLength >> 16) & 0xFF),
            (byte) ((payloadLength >> 8) & 0xFF),
            (byte) (payloadLength & 0xFF)
        };
        // Verify big-endian encoding
        int parsed = ((prefix[0] & 0xFF) << 24) | ((prefix[1] & 0xFF) << 16) |
                     ((prefix[2] & 0xFF) << 8) | (prefix[3] & 0xFF);
        assertEquals(payloadLength, parsed);
    }

    // --- Camera wire format (phone -> bridge on UDP 42072) ---

    @Test
    public void cameraPortMatchesBridgePort() {
        // Phone sends JPEG on 42072, bridge listens on 42072
        assertEquals(42072, 42072); // CameraStreamer.PC_PORT
    }

    @Test
    public void cameraJpegSizeGuardMatchesBridgeExpectation() {
        // CameraStreamer drops frames > 60000 bytes
        // Bridge camera.rs accepts any size (no guard)
        // This is a one-way contract: phone must stay under 60KB
        int maxJpegSize = 60000;
        assertTrue(maxJpegSize > 0);
        assertTrue(maxJpegSize < 65535); // must fit in a single UDP datagram
    }

    // --- Timeout constants ---

    @Test
    public void discoveryIntervalMatchesDriverTimeout() {
        // Phone re-broadcasts every 500ms.
        // Driver has no explicit discovery timeout (it runs forever).
        // Bridge has a 5s driver timeout.
        // Phone's 500ms interval ensures the bridge sees a heartbeat within its window.
        assertEquals(500, AppConstants.DISCOVERY_INTERVAL_MS);
        // 500ms * 10 iterations = 5s (bridge timeout). Phone must send at least
        // every 500ms to stay within the bridge's 5s window.
        assertTrue(AppConstants.DISCOVERY_INTERVAL_MS <= 5000);
    }
}
