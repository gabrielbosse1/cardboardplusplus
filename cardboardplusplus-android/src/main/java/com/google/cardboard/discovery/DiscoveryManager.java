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
 * <p>After the first ACK the loop backs off to a 1-per-5s heartbeat (never stops:
 * a restarted driver/SteamVR clears its phone target and needs a new discovery
 * packet to relearn it). Full rate resumes after 5s without an ACK or on a
 * watchdog stall via {@link #pokeNow()}. The manager re-sends
 * {@code CARDBOARD_CAP <W> <H>} about every 30s (every 60th ACK) using the
 * same socket that proved connectivity (avoids Windows firewall dropping packets
 * from a new socket), so a restarted driver always learns this phone's decode ceiling.
 *
 * <p>The wire values below are part of the runtime protocol shared with the PC driver and must not
 * change.
 */
public class DiscoveryManager {
  private static final String TAG = DiscoveryManager.class.getSimpleName();
  private static final DebugLog DBG = new DebugLog(TAG);
  // Wire protocol strings live in AppConstants (locked values shared with the
  // driver); used here as DISCOVERY_MESSAGE / ACK_RESPONSE / CAP_PREFIX.
  private static final String DISCOVERY_MESSAGE = AppConstants.DISCOVERY_MESSAGE;
  private static final String ACK_RESPONSE = AppConstants.DISCOVERY_ACK;
  private static final String CAP_PREFIX = AppConstants.CAP_PREFIX;
  private static final int CAP_SEND_ATTEMPTS = 3;
  private static final long CAP_SEND_GAP_MS = 500;

  // Post-ACK heartbeat: 1 per 5s. No ACK for this long (or pre-first-ACK)
  // means full-rate broadcast.
  static final long HEARTBEAT_INTERVAL_MS = 5000;

  private final AppSettings appSettings;

  private static final int FALLBACK_AFTER_FAILURES = 5;

  private volatile boolean broadcasting = false;
  private Thread discoveryThread = null;
  // Live socket + last driver address for pokeNow(); written by the discovery
  // thread, read by the watchdog thread.
  private volatile DatagramSocket liveSocket;
  private volatile InetAddress lastDriverAddr;
  // Set from a background thread (decoder-cap query); read by discovery thread.
  private volatile int capWidth;
  private volatile int capHeight;
  // Cached broadcast target: re-resolved only when the configured IP changes.
  private String cachedIp;
  private InetAddress cachedAddr;

  public DiscoveryManager(AppSettings appSettings) {
    this.appSettings = appSettings;
  }

  /**
   * Set the hardware decoder cap dimensions to announce to the PC after discovery succeeds.
   * Must be called before {@link #startDiscovery()}. Zero (unset) is never announced.
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

  /**
   * Single discovery + CAP probe on the live discovery socket (no loop, no
   * sleep). Called by the video watchdog on stall; falls back to full
   * {@link #startDiscovery()} when no discovery socket is live.
   */
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

  public void stopDiscovery() {
    broadcasting = false;
    Thread t = discoveryThread;
    discoveryThread = null;
    liveSocket = null;
    if (t != null) {
      // Never join on the caller (often the UI thread): interrupt the loop
      // and reap it on a daemon thread.
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

  /**
   * Broadcasts the discovery message until {@link #stopDiscovery()} flips the
   * running flag. Full rate (500ms) until the first ACK, then a 1-per-5s
   * heartbeat; full rate resumes after 5s without an ACK. Between polls the
   * socket times out (so the flag is re-checked) rather than blocking
   * indefinitely.
   */
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
            // Re-announce the hardware decoder cap ~every 30s (every 60th ACK)
            // on the same socket that proved connectivity (avoids Windows
            // firewall dropping packets from a brand-new socket). A one-shot
            // CAP is lost forever if the driver restarts afterwards and keeps
            // encoding above this phone's decode ceiling (black screen).
            if (capWidth > 0 && capHeight > 0 && ackCount % 60 == 1) {
              // Send CAP to the driver's actual IP (from ACK response), NOT the
              // broadcast address. Broadcast CAP packets are silently dropped by
              // Windows Firewall as unsolicited inbound, so the driver never
              // receives them and the encoder runs unclamped. The 3x burst runs
              // on a short-lived thread so the discovery loop never stalls.
              final InetAddress driverAddr = recvPacket.getAddress();
              Thread capThread = new Thread(() -> sendCapBurst(socket, driverAddr));
              capThread.setDaemon(true);
              capThread.start();
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

        if (!broadcasting) break;
        // Back off to a heartbeat after the first ACK; resume full rate when
        // the ACKs go quiet (driver restarted underneath us).
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

  /** Resolve the discovery target, caching until the configured IP changes. */
  private synchronized InetAddress resolveTarget(String configuredIp) throws Exception {
    String key = (configuredIp != null) ? configuredIp.trim() : "";
    if (cachedAddr == null || !key.equals(cachedIp)) {
      cachedAddr = NetworkUtils.getPcOrBroadcastAddress(configuredIp);
      cachedIp = key;
    }
    return cachedAddr;
  }

  /** Send CARDBOARD_CAP in a burst on the proven discovery socket (blocks ~1.5s; run off-thread). */
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
