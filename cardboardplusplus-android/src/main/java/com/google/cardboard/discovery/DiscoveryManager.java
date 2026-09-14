package com.google.cardboard.discovery;

import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * UDP broadcast discovery of the paired PC driver.
 *
 * <p>On {@link #startDiscovery()} the app repeatedly broadcasts {@code CARDBOARD_DISCOVERY} to either
 * the configured PC IP or the subnet broadcast address until the driver answers {@code ACK}. The
 * driver uses a discovery packet as the signal that this phone is alive and should receive video.
 *
 * <p>After receiving the first ACK, the manager also sends {@code CARDBOARD_CAP <W> <H>} using the
 * same socket that proved connectivity (avoids Windows firewall dropping packets from a new socket).
 *
 * <p>The wire values below are part of the runtime protocol shared with the PC driver and must not
 * change.
 */
public class DiscoveryManager {
  private static final String TAG = DiscoveryManager.class.getSimpleName();
  private static final DebugLog DBG = DebugLog.create(DiscoveryManager.class, null);
  // Wire protocol strings (shared with the driver) - change only together with the driver side.
  private static final String DISCOVERY_MESSAGE = "CARDBOARD_DISCOVERY";
  private static final String ACK_RESPONSE = "ACK";
  private static final String CAP_PREFIX = "CARDBOARD_CAP ";
  private static final int CAP_SEND_ATTEMPTS = 3;
  private static final long CAP_SEND_GAP_MS = 500;

  private final AppSettings appSettings;

  private static final int FALLBACK_AFTER_FAILURES = 5;

  private volatile boolean broadcasting = false;
  private Thread discoveryThread = null;
  private int capWidth;
  private int capHeight;

  public DiscoveryManager(AppSettings appSettings) {
    this.appSettings = appSettings;
    DBG.setEnabled(appSettings.isDebugLogging());
  }

  /**
   * Set the hardware decoder cap dimensions to announce to the PC after discovery succeeds.
   * Must be called before {@link #startDiscovery()}.
   */
  public void setDecoderCap(int width, int height) {
    this.capWidth = width;
    this.capHeight = height;
  }

  public void startDiscovery() {
    if (discoveryThread != null && discoveryThread.isAlive()) {
      return;
    }
    broadcasting = true;
    discoveryThread =
        new Thread(
            () -> {
              try (DatagramSocket socket = new DatagramSocket()) {
                socket.setBroadcast(true);
                socket.setSoTimeout(1000);
                broadcastUntilAck(
                    socket, DISCOVERY_MESSAGE.getBytes(), new byte[64]);
              } catch (Exception e) {
                Log.e(TAG, "Discovery error: " + e.getMessage());
              }
            });
    discoveryThread.start();
  }

  public void stopDiscovery() {
    broadcasting = false;
    if (discoveryThread != null) {
      try {
        discoveryThread.join(2000);
      } catch (InterruptedException e) {
        // Interrupted while joining; the broadcast loop ends on the next timeout anyway.
      }
      discoveryThread = null;
    }
  }

  /**
   * Broadcasts the discovery message until the driver ACKs or {@link #stopDiscovery()} flips the
   * running flag. Between polls the socket times out (so the flag is re-checked) rather than
   * blocking indefinitely.
   */
  private void broadcastUntilAck(DatagramSocket socket, byte[] sendData, byte[] recvBuffer) {
    boolean capSent = false;
    int consecutiveFailures = 0;
    try {
      while (broadcasting) {
        String configuredIp = appSettings.getPcIp();
        InetAddress target = NetworkUtils.getPcOrBroadcastAddress(configuredIp);
        DatagramPacket sendPacket =
            new DatagramPacket(
                sendData,
                sendData.length,
                target,
                AppConstants.UDP_DISCOVERY_PORT);
        socket.send(sendPacket);
        DBG.d("Discovery sent to %s", sendPacket.getAddress().getHostAddress());

        try {
          DatagramPacket recvPacket = new DatagramPacket(recvBuffer, recvBuffer.length);
          socket.receive(recvPacket);
          String response = new String(recvPacket.getData(), 0, recvPacket.getLength());
          DBG.d("Discovery response: %s from %s", response, recvPacket.getAddress());
          if (ACK_RESPONSE.equals(response)) {
            Log.i(TAG, "Discovery successful, driver connected");
            consecutiveFailures = 0;
            // Send the hardware decoder cap on the same socket that proved
            // connectivity (avoids Windows firewall dropping packets from a
            // brand-new socket to the same port).
            if (!capSent && capWidth > 0 && capHeight > 0) {
              sendCap(socket, sendPacket.getAddress());
              capSent = true;
            }
          }
        } catch (Exception e) {
          // Timeout — no ACK yet.
          if (configuredIp != null && !configuredIp.isEmpty()) {
            consecutiveFailures++;
            if (consecutiveFailures >= FALLBACK_AFTER_FAILURES) {
              Log.w(TAG, "No ACK from " + configuredIp + " after " + consecutiveFailures
                  + " attempts, falling back to broadcast discovery");
              appSettings.setPcIp("");
              consecutiveFailures = 0;
            }
          }
        }

        if (broadcasting) {
          Thread.sleep(AppConstants.DISCOVERY_INTERVAL_MS);
        }
      }
    } catch (Exception e) {
      Log.e(TAG, "Discovery error: " + e.getMessage());
    }
  }

  /** Send CARDBOARD_CAP on the proven discovery socket. */
  private void sendCap(DatagramSocket socket, InetAddress driverAddr) {
    String msg = CAP_PREFIX + capWidth + " " + capHeight;
    byte[] data = msg.getBytes();
    for (int i = 0; i < CAP_SEND_ATTEMPTS; i++) {
      try {
        socket.send(
            new DatagramPacket(data, data.length, driverAddr, AppConstants.UDP_DISCOVERY_PORT));
        DBG.i("Sent decoder cap to %s: %s", driverAddr.getHostAddress(), msg);
        Thread.sleep(CAP_SEND_GAP_MS);
      } catch (Exception e) {
        Log.w(TAG, "Failed to send decoder cap", e);
        break;
      }
    }
  }
}