#pragma once
#ifndef CBPP_NETWORK_CLIENT_H
#define CBPP_NETWORK_CLIENT_H

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>

// POSIX sockets (Android/Linux)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "Protocol.h"

namespace cbpp {

class NetworkClient {
public:
    NetworkClient();
    ~NetworkClient();

    // Start all network threads. Call once on app init.
    void Start();

    // Stop all network threads. Call on app destroy.
    void Stop();

    // Is the driver connected?
    bool IsDriverConnected() const { return m_driverConnected; }

    // Get driver IP string (for logging/UI)
    std::string GetDriverIP() const { return m_driverIP; }

    // ---- Tracking ----
    // Send orientation + position to driver. Call from GL thread or sensor callback.
    void SendTracking(float oriW, float oriX, float oriY, float oriZ,
                      float posX, float posY, float posZ);

    // ---- Camera (placeholder) ----
    // TODO: send camera frame to driver
    void SendCameraChunk(const uint8_t* data, uint32_t size,
                         uint32_t frameId, uint32_t chunkIndex,
                         uint32_t totalChunks, uint32_t width, uint32_t height);

    // ---- Video receive callback ----
    // Set callback for received video chunks. Will be called from network thread.
    // Signature: void(const uint8_t* payload, int payloadSize, const VideoChunkHeader& header)
    using VideoChunkCallback = std::function<void(const uint8_t*, int, const VideoChunkHeader&)>;
    void SetVideoChunkCallback(VideoChunkCallback cb) { m_videoChunkCallback = cb; }

private:
    void DiscoveryLoop();
    void VideoReceiveLoop();
    void TrackingSendLoop();  // not needed for send-only, but kept for future heartbeat

    // Sockets
    int m_discoverySocket;
    int m_videoSocket;
    int m_trackingSocket;
    int m_cameraSocket;

    // Threads
    std::thread m_discoveryThread;
    std::thread m_videoThread;

    // State
    std::atomic<bool> m_running;
    std::atomic<bool> m_driverConnected;
    std::string m_driverIP;
    sockaddr_in m_driverAddr;  // cached for sending tracking/camera
    std::mutex m_driverMutex;

    // Tracking
    std::mutex m_trackingMutex;
    TrackingPayload m_lastTracking;

    // Video callback
    VideoChunkCallback m_videoChunkCallback;
};

}  // namespace cbpp

#endif  // CBPP_NETWORK_CLIENT_H
