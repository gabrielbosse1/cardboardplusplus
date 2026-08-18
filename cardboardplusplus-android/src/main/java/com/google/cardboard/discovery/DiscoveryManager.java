package com.google.cardboard.discovery;

import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;

/**
 * UDP broadcast discovery of the paired PC driver.
 *
 * <p>On {@link #startDiscovery()} the app repeatedly broadcasts {@code CARDBOARD_DISCOVERY} to either
 * the configured PC IP or the subnet broadcast address until the driver answers {@code ACK}. The
 * driver uses a discovery packet as the signal that this phone is alive and should receive video.
 *
 * <p>The wire values below are part of the runtime protocol shared with the PC driver and must not
 * change.
 */
public class DiscoveryManager {
  private static final String TAG = DiscoveryManager.class.getSimpleName();
  // Wire protocol strings (shared with the driver) - change only together with the driver side.
  private static final String DISCOVERY_MESSAGE = "CARDBOARD_DISCOVERY";
  private static final String ACK_RESPONSE = "ACK";

  private final AppSettings appSettings;

  private volatile boolean broadcasting = false;
  private Thread discoveryThread = null;

  public DiscoveryManager(AppSettings appSettings) {
    this.appSettings = appSettings;
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
    try {
      while (broadcasting) {
        DatagramPacket sendPacket =
            new DatagramPacket(
                sendData,
                sendData.length,
                NetworkUtils.getPcOrBroadcastAddress(appSettings.getPcIp()),
                AppConstants.UDP_DISCOVERY_PORT);
        socket.send(sendPacket);
        Log.d(TAG, "Discovery sent to " + sendPacket.getAddress().getHostAddress());

        try {
          DatagramPacket recvPacket = new DatagramPacket(recvBuffer, recvBuffer.length);
          socket.receive(recvPacket);
          String response = new String(recvPacket.getData(), 0, recvPacket.getLength());
          Log.d(
              TAG,
              "Discovery response: " + response + " from " + recvPacket.getAddress());
          if (ACK_RESPONSE.equals(response)) {
            broadcasting = false;
            Log.i(TAG, "Discovery successful, driver connected");
            break;
          }
        } catch (Exception e) {
          // Timeout or no answer yet - keep broadcasting.
        }

        if (broadcasting) {
          Thread.sleep(AppConstants.DISCOVERY_INTERVAL_MS);
        }
      }
    } catch (Exception e) {
      Log.e(TAG, "Discovery error: " + e.getMessage());
    }
  }
}