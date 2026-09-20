package com.google.cardboard.network;
import android.content.Context;
import android.net.DhcpInfo;
import android.net.wifi.WifiManager;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
// Address helpers in network/; DiscoveryManager, TelemetrySender, and CameraStreamer resolve targets through it.
public final class NetworkUtils {
  private NetworkUtils() {}
  private static volatile Context appContext;
  // Caches the application context for WiFi lookups; called from VrActivity.onCreate on the UI thread.
  public static void init(Context context) {
    appContext = (context != null) ? context.getApplicationContext() : null;
  }
  // Returns the directed broadcast address with global fallback; called from discovery and streamer targeting.
  public static InetAddress getBroadcastAddress() throws UnknownHostException {
    InetAddress directed = getDirectedBroadcastAddress();
    return (directed != null) ? directed : InetAddress.getByName("255.255.255.255");
  }
  // Derives the subnet broadcast from DHCP info; called from getBroadcastAddress with null on failure.
  private static InetAddress getDirectedBroadcastAddress() {
    try {
      Context ctx = appContext;
      if (ctx == null) return null;
      WifiManager wm = (WifiManager) ctx.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
      if (wm == null) return null;
      DhcpInfo dhcp = wm.getDhcpInfo();
      if (dhcp == null || dhcp.ipAddress == 0) return null;
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
  // Returns the configured PC address or broadcast; called from discovery, telemetry, and streamer targeting.
  public static InetAddress getPcOrBroadcastAddress(String pcIp) throws UnknownHostException {
    if (pcIp != null && !pcIp.trim().isEmpty()) {
      return preferIPv4(pcIp.trim());
    }
    return getBroadcastAddress();
  }
  // Picks the IPv4 result for a hostname; called from getPcOrBroadcastAddress during targeting.
  static InetAddress preferIPv4(String host) throws UnknownHostException {
    InetAddress[] all = InetAddress.getAllByName(host);
    for (InetAddress a : all) {
      if (a instanceof Inet4Address) return a;
    }
    return all[0];
  }
}
