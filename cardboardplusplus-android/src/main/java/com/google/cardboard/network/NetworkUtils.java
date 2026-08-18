package com.google.cardboard.network;

import java.net.InetAddress;
import java.net.UnknownHostException;

/**
 * Networking static helpers for PC discovery.
 *
 * <p>This package owns everything that decides, per broadcast cycle, who the phone talks to over
 * UDP: the discovery broadcast address and the configured-PC-IP-vs-broadcast decision.
 */
public final class NetworkUtils {
  private NetworkUtils() {}

  /** Broadcast address used for PC discovery. */
  public static InetAddress getBroadcastAddress() throws UnknownHostException {
    return InetAddress.getByName("255.255.255.255");
  }

  /** Return the configured PC IP if non-empty, otherwise fall back to broadcast. */
  public static InetAddress getPcOrBroadcastAddress(String pcIp) throws UnknownHostException {
    if (pcIp != null && !pcIp.trim().isEmpty()) {
      return InetAddress.getByName(pcIp.trim());
    }
    return getBroadcastAddress();
  }
}