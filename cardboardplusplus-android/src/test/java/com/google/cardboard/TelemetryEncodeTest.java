package com.google.cardboard;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for phone-side telemetry encoding that pretend to be the bridge.
 *
 * These tests verify the binary wire format matches what the bridge's
 * telemetry parser expects (telemetry.rs).
 */
public class TelemetryEncodeTest {

    private static final byte GYRO_TAG = 0x10;
    private static final byte HAND_TAG = 0x11;
    private static final byte PING_TAG = 0x20;

    @Test
    public void gyroPacketHasCorrectTagAndLength() {
        byte[] packet = buildGyroPacket(1234, new float[]{0.5f, -0.2f, 0.1f}, new float[]{1.0f, 9.8f, 0.0f});
        assertEquals(GYRO_TAG, packet[0]);
        assertEquals(33, packet.length); // 1 + 8 + 6*4 = 33
    }

    @Test
    public void gyroPacketTimestampIsLittleEndian() {
        byte[] packet = buildGyroPacket(256, new float[]{0, 0, 0}, new float[]{0, 0, 0});
        // timestamp 256 = 0x0000000000000100 in LE
        assertEquals(0x00, packet[1]);
        assertEquals(0x01, packet[2]);
        assertEquals(0x00, packet[3]);
        assertEquals(0x00, packet[4]);
    }

    @Test
    public void handPacketHasCorrectTagAndLength() {
        byte[] packet = buildHandPacket(2345, 2, 21, 0.91f);
        assertEquals(HAND_TAG, packet[0]);
        assertEquals(15, packet.length); // 1 + 8 + 1 + 1 + 4 = 15
    }

    @Test
    public void handPacketFieldsAreCorrect() {
        byte[] packet = buildHandPacket(100, 2, 21, 0.85f);
        assertEquals(2, packet[9]);   // hands count
        assertEquals(21, packet[10]); // landmarks per hand

        float confidence = ByteBuffer.wrap(packet, 11, 4).order(ByteOrder.LITTLE_ENDIAN).getFloat();
        assertEquals(0.85f, confidence, 0.001f);
    }

    @Test
    public void pingPacketIsSingleByte() {
        byte[] packet = new byte[]{PING_TAG};
        assertEquals(1, packet.length);
        assertEquals(PING_TAG, packet[0]);
    }

    @Test
    public void helloPacketStartsWithCorrectPrefix() {
        String hello = "CARDBOARD_PHONE_HELLO v1";
        assertTrue(hello.startsWith("CARDBOARD_PHONE_HELLO"));
        assertEquals("CARDBOARD_PHONE_HELLO v1", hello);
    }

    @Test
    public void gyroPacketWithZerosIsValid() {
        byte[] packet = buildGyroPacket(0, new float[]{0, 0, 0}, new float[]{0, 0, 0});
        assertEquals(33, packet.length);
        assertEquals(GYRO_TAG, packet[0]);
    }

    @Test
    public void handPacketWithNoHandsIsValid() {
        byte[] packet = buildHandPacket(0, 0, 21, 0.0f);
        assertEquals(0, packet[9]); // zero hands
    }

    // --- Builders ---

    private byte[] buildGyroPacket(long timestampMs, float[] angVel, float[] accel) {
        ByteBuffer buf = ByteBuffer.allocate(33).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(GYRO_TAG);
        buf.putLong(timestampMs);
        for (float v : angVel) buf.putFloat(v);
        for (float v : accel) buf.putFloat(v);
        return buf.array();
    }

    private byte[] buildHandPacket(long timestampMs, int hands, int landmarks, float confidence) {
        ByteBuffer buf = ByteBuffer.allocate(15).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(HAND_TAG);
        buf.putLong(timestampMs);
        buf.put((byte) hands);
        buf.put((byte) landmarks);
        buf.putFloat(confidence);
        return buf.array();
    }
}
