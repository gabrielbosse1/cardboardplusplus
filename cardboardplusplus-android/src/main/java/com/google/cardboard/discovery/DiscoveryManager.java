package com.google.cardboard.discovery;

import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.util.NetworkUtils;
import java.net.DatagramPacket;
import java.net.DatagramSocket;

/** UDP broadcast discovery of the paired PC driver. */
public class DiscoveryManager {
  private static final String TAG = DiscoveryManager.class.getSimpleName();
  private static final String DISCOVERY_MESSAGE = "CARDBOARD_DISCOVERY";
  private static final String ACK = "ACK";

  private volatile boolean discoveryRunning = false;
  private Thread discoveryThread = null;

  public void startDiscovery() {
    if (discoveryThread != null && discoveryThread.isAlive()) {
      return;
    }
    discoveryRunning = true;
    discoveryThread =
        new Thread(
            () -> {
              try {
                DatagramSocket socket = new DatagramSocket();
                socket.setBroadcast(true);
                socket.setSoTimeout(1000);

                byte[] sendData = DISCOVERY_MESSAGE.getBytes();
                byte[] recvBuffer = new byte[64];

                while (discoveryRunning) {
                  DatagramPacket sendPacket =
                      new DatagramPacket(
                          sendData,
                          sendData.length,
                          NetworkUtils.getBroadcastAddress(),
                          AppConstants.UDP_DISCOVERY_PORT);
                  socket.send(sendPacket);
                  Log.d(TAG, "Discovery broadcast sent");

                  try {
                    DatagramPacket recvPacket =
                        new DatagramPacket(recvBuffer, recvBuffer.length);
                    socket.receive(recvPacket);
                    String response =
                        new String(recvPacket.getData(), 0, recvPacket.getLength());
                    Log.d(
                        TAG,
                        "Discovery response: " + response + " from " + recvPacket.getAddress());
                    if (ACK.equals(response)) {
                      discoveryRunning = false;
                      Log.i(TAG, "Discovery successful, driver connected");
                      break;
                    }
                  } catch (Exception e) {
                    // Timeout or no answer yet - keep broadcasting.
                  }

                  if (discoveryRunning) {
                    Thread.sleep(AppConstants.DISCOVERY_INTERVAL_MS);
                  }
                }

                socket.close();
              } catch (Exception e) {
                Log.e(TAG, "Discovery error: " + e.getMessage());
              }
            });
    discoveryThread.start();
  }

  public void stopDiscovery() {
    discoveryRunning = false;
    if (discoveryThread != null) {
      try {
        discoveryThread.join(2000);
      } catch (InterruptedException e) {
      }
      discoveryThread = null;
    }
  }
}
