package com.google.cardboard.network;

import android.content.Context;
import android.net.DhcpInfo;
import android.net.wifi.WifiManager;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;

/**
 * Networking static helpers for PC discovery.
 *
 * <p>This package owns everything that decides, per broadcast cycle, who the phone talks to over
 * UDP: the discovery broadcast address and the configured-PC-IP-vs-broadcast decision.
 *
 * <p>Note on Doze: the app holds no foreground service and requests no battery
 * exemption by design — while backgrounded, Doze may throttle UDP/sensors. The
 * WiFi/sensor paths are only needed in-viewer (activity resumed), where the
 * wake + WiFi locks in {@code VrActivity} already apply.
 */
public final class NetworkUtils {
  private NetworkUtils() {}

  // Application context for subnet-directed broadcast (set once in VrActivity).
  private static volatile Context appContext;

  /** Cache the application context used to resolve the subnet broadcast address. */
  public static void init(Context context) {
    appContext = (context != null) ? context.getApplicationContext() : null;
  }

  /**
   * Subnet-directed broadcast (e.g. 192.168.1.255) derived from the WiFi
   * DHCP info. Falls back to the global broadcast address when WiFi/DHCP
   * info is unavailable (mobile data, VPN/Tailscale with no broadcast
   * domain). Directed broadcast survives AP client-isolation and subnets
   * where 255.255.255.255 is dropped.
   */
  public static InetAddress getBroadcastAddress() throws UnknownHostException {
    InetAddress directed = getDirectedBroadcastAddress();
    return (directed != null) ? directed : InetAddress.getByName("255.255.255.255");
  }

  private static InetAddress getDirectedBroadcastAddress() {
    try {
      Context ctx = appContext;
      if (ctx == null) return null;
      WifiManager wm = (WifiManager) ctx.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
      if (wm == null) return null;
      DhcpInfo dhcp = wm.getDhcpInfo();
      if (dhcp == null || dhcp.ipAddress == 0) return null;
      // DhcpInfo fields are little-endian ints; intToIp expects network order.
      int broadcast = (dhcp.ipAddress & dhcp.netmask) | ~dhcp.netmask;
      byte[] addr = new byte[] {
        (byte) (broadcast & 0xFF),
        (byte) ((broadcast >> 8) & 0xFF),
        (byte) ((broadcast >> 16) & 0xFF),
        (byte) ((broadcast >> 24) & 0xFF),
      };
      return InetAddress.getByAddress(addr);
    } catch (Exception e) {
      return null;
    }
  }

  /**
   * Return the configured PC IP if non-empty (unicast path, unchanged),
   * otherwise fall back to broadcast. Hostnames resolve preferring IPv4 —
   * the native video receiver only speaks AF_INET.
   */
  public static InetAddress getPcOrBroadcastAddress(String pcIp) throws UnknownHostException {
    if (pcIp != null && !pcIp.trim().isEmpty()) {
      return preferIPv4(pcIp.trim());
    }
    return getBroadcastAddress();
  }

  /** Resolve preferring the first IPv4 result; fall back to the default resolution. */
  static InetAddress preferIPv4(String host) throws UnknownHostException {
    InetAddress[] all = InetAddress.getAllByName(host);
    for (InetAddress a : all) {
      if (a instanceof Inet4Address) return a;
    }
    return all[0];
  }
}
