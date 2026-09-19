package com.google.cardboard;

import com.google.cardboard.core.AppConstants;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.junit.Test;
import static org.junit.Assert.*;

/**
 * Tests for the phone-side net-stats packet (tag 0x13) that pretend to be the bridge.
 *
 * <p>Mirrors {@code NetStatsReporter.buildPacket}: [0x13][u64 ts LE][u32 frames LE][u32 stalls
 * LE][f32 fps LE] = 21 bytes. Must match {@code telemetry.rs} tag 0x13 parsing.
 */
public class NetStatsTest {

    private static final byte NET_STATS_TAG = AppConstants.TELEMETRY_TAG_NETSTATS;

    private static byte[] buildNetStats(long timestampMs, int frames, int stalls, float fps) {
        ByteBuffer buf = ByteBuffer.allocate(21).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(NET_STATS_TAG);
        buf.putLong(timestampMs);
        buf.putInt(frames);
        buf.putInt(stalls);
        buf.putFloat(fps);
        return buf.array();
    }

    @Test
    public void netStatsPacketHasCorrectTagAndLength() {
        byte[] packet = buildNetStats(1234, 60, 0, 30.0f);
        assertEquals(NET_STATS_TAG, packet[0]);
        assertEquals(21, packet.length); // 1 + 8 + 4 + 4 + 4 = 21
    }

    @Test
    public void netStatsFieldsAreLittleEndian() {
        byte[] packet = buildNetStats(256, 0x01020304, 2, 30.0f);
        assertEquals(0x00, packet[1]);
        assertEquals(0x01, packet[2]);
        int frames = ByteBuffer.wrap(packet, 9, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
        assertEquals(0x01020304, frames);
        int stalls = ByteBuffer.wrap(packet, 13, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
        assertEquals(2, stalls);
    }

    @Test
    public void netStatsTagDoesNotCollideWithTelemetryTags() {
        assertNotEquals((byte) 0x10, NET_STATS_TAG);
        assertNotEquals((byte) 0x11, NET_STATS_TAG);
        assertNotEquals((byte) 0x12, NET_STATS_TAG);
        assertNotEquals((byte) 0x20, NET_STATS_TAG);
    }
}
