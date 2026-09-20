package com.google.cardboard.discovery;
import android.util.Log;
import com.google.cardboard.core.AppConstants;
import com.google.cardboard.core.DebugLog;
import com.google.cardboard.network.NetworkUtils;
import com.google.cardboard.settings.AppSettings;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
// UDP discovery owner in discovery/; VrActivity starts/stops it and VideoManager pokes it on video stall.
public class DiscoveryManager {
  private static final String TAG = DiscoveryManager.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  private static final String DISCOVERY_MESSAGE = AppConstants.DISCOVERY_MESSAGE;
  private static final String ACK_RESPONSE = AppConstants.DISCOVERY_ACK;
  private static final String CAP_PREFIX = AppConstants.CAP_PREFIX;
  private static final int CAP_SEND_ATTEMPTS = 3;
  private static final long CAP_SEND_GAP_MS = 500;
  static final long HEARTBEAT_INTERVAL_MS = 5000;
  private final AppSettings appSettings;
  private static final int FALLBACK_AFTER_FAILURES = 5;
  private volatile boolean broadcasting = false;
  private Thread discoveryThread = null;
  private volatile DatagramSocket liveSocket;
  private volatile InetAddress lastDriverAddr;
  private volatile int capWidth;
  private volatile int capHeight;
  private String cachedIp;
  private InetAddress cachedAddr;
  // Stores settings for PC-IP resolution; called from VrActivity.onCreate on the UI thread.
  public DiscoveryManager(AppSettings appSettings) {
    this.appSettings = appSettings;
  }
  // Caches the hardware decode size for CAP announcements; called from VrActivity's decoder-cap thread.
  public void setDecoderCap(int width, int height) {
    this.capWidth = width;
    this.capHeight = height;
  }
  // Starts the broadcast-until-ACK loop; called from VrActivity startSession on the UI thread.
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
                liveSocket = socket;
                try {
                  broadcastUntilAck(
                      socket, DISCOVERY_MESSAGE.getBytes(), new byte[64]);
                } finally {
                  if (liveSocket == socket) liveSocket = null;
                }
              } catch (Exception e) {
                Log.e(TAG, "Discovery error: " + e.getMessage());
              }
            });
    discoveryThread.start();
  }
  // Sends one discovery plus CAP burst on the live socket; called from VideoManager's watchdog reconnect hook.
  public void pokeNow() {
    DatagramSocket socket = liveSocket;
    InetAddress driverAddr = lastDriverAddr;
    if (socket == null || socket.isClosed()) {
      startDiscovery();
      return;
    }
    try {
      String configuredIp = appSettings.getPcIp();
      InetAddress target = resolveTarget(configuredIp);
      byte[] discovery = DISCOVERY_MESSAGE.getBytes();
      socket.send(new DatagramPacket(discovery, discovery.length, target,
          AppConstants.UDP_DISCOVERY_PORT));
      InetAddress capTarget = (driverAddr != null) ? driverAddr : target;
      sendCapBurst(socket, capTarget);
    } catch (Exception e) {
      Log.w(TAG, "Discovery poke failed: " + e.getMessage());
    }
  }
  // Halts broadcasting and joins the worker; called from VrActivity.onPause on the UI thread.
  public void stopDiscovery() {
    broadcasting = false;
    Thread t = discoveryThread;
    discoveryThread = null;
    liveSocket = null;
    if (t != null) {
      t.interrupt();
      Thread reaper = new Thread(
          () -> {
            try {
              t.join(2000);
            } catch (InterruptedException ignored) {
            }
          });
      reaper.setDaemon(true);
      reaper.start();
    }
  }
  // Broadcasts discovery and backs off to heartbeat after ACK; runs on the discovery thread.
  private void broadcastUntilAck(DatagramSocket socket, byte[] sendData, byte[] recvBuffer) {
    int ackCount = 0;
    int consecutiveFailures = 0;
    long lastAckMs = 0;
    try {
      while (broadcasting) {
        String configuredIp = appSettings.getPcIp();
        InetAddress target = resolveTarget(configuredIp);
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
            lastAckMs = System.currentTimeMillis();
            lastDriverAddr = recvPacket.getAddress();
            ackCount++;
            if (capWidth > 0 && capHeight > 0 && ackCount % 60 == 1) {
              final InetAddress driverAddr = recvPacket.getAddress();
              Thread capThread = new Thread(() -> sendCapBurst(socket, driverAddr));
              capThread.setDaemon(true);
              capThread.start();
            }
          }
        } catch (Exception e) {
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
        if (!broadcasting) break;
        boolean ackedRecently = ackCount > 0
            && (System.currentTimeMillis() - lastAckMs) < HEARTBEAT_INTERVAL_MS;
        try {
          Thread.sleep(ackedRecently ? HEARTBEAT_INTERVAL_MS
              : AppConstants.DISCOVERY_INTERVAL_MS);
        } catch (InterruptedException e) {
          break;
        }
      }
    } catch (Exception e) {
      Log.e(TAG, "Discovery error: " + e.getMessage());
    }
  }
  // Resolves and caches the unicast-or-broadcast target; called from the discovery thread and pokeNow.
  private synchronized InetAddress resolveTarget(String configuredIp) throws Exception {
    String key = (configuredIp != null) ? configuredIp.trim() : "";
    if (cachedAddr == null || !key.equals(cachedIp)) {
      cachedAddr = NetworkUtils.getPcOrBroadcastAddress(configuredIp);
      cachedIp = key;
    }
    return cachedAddr;
  }
  // Sends the decoder cap burst to the driver; called from the ACK path and pokeNow.
  private void sendCapBurst(DatagramSocket socket, InetAddress driverAddr) {
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
