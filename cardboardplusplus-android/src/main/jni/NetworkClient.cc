#include "NetworkClient.h"
#include <android/log.h>

#define LOG_TAG "CBPP_Net"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace cbpp {

NetworkClient::NetworkClient()
    : m_discoverySocket(-1)
    , m_videoSocket(-1)
    , m_trackingSocket(-1)
    , m_cameraSocket(-1)
    , m_running(false)
    , m_driverConnected(false)
    , m_lastTracking{}
{}

NetworkClient::~NetworkClient() {
    Stop();
}

void NetworkClient::Start() {
    if (m_running) return;
    m_running = true;

    LOGD("NetworkClient starting...");

    // Start discovery listener thread
    m_discoveryThread = std::thread(&NetworkClient::DiscoveryLoop, this);

    // Start video receive thread
    m_videoThread = std::thread(&NetworkClient::VideoReceiveLoop, this);

    LOGD("NetworkClient started");
}

void NetworkClient::Stop() {
    if (!m_running) return;
    m_running = false;

    LOGD("NetworkClient stopping...");

    // Close sockets to unblock threads
    if (m_discoverySocket >= 0) { close(m_discoverySocket); m_discoverySocket = -1; }
    if (m_videoSocket >= 0) { close(m_videoSocket); m_videoSocket = -1; }
    if (m_trackingSocket >= 0) { close(m_trackingSocket); m_trackingSocket = -1; }
    if (m_cameraSocket >= 0) { close(m_cameraSocket); m_cameraSocket = -1; }

    if (m_discoveryThread.joinable()) m_discoveryThread.join();
    if (m_videoThread.joinable()) m_videoThread.join();

    m_driverConnected = false;
    LOGD("NetworkClient stopped");
}

// ============================================================
// Discovery: listen for PC broadcast announcements
// ============================================================
void NetworkClient::DiscoveryLoop() {
    LOGD("Discovery loop starting on port %d", PORT_BROADCAST);

    m_discoverySocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_discoverySocket < 0) {
        LOGE("Discovery socket() failed: %s", strerror(errno));
        return;
    }

    int reuse = 1;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(PORT_BROADCAST);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_discoverySocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        LOGE("Discovery bind() failed: %s", strerror(errno));
        close(m_discoverySocket);
        m_discoverySocket = -1;
        return;
    }

    // Set timeout so we can check m_running periodically
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500ms
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LOGD("Discovery listening on port %d", PORT_BROADCAST);

    uint8_t buf[1024];
    sockaddr_in fromAddr = {};
    socklen_t fromLen = sizeof(fromAddr);

    while (m_running) {
        int received = recvfrom(m_discoverySocket, buf, sizeof(buf), 0,
                                (sockaddr*)&fromAddr, &fromLen);

        if (received < (int)sizeof(PacketHeader)) {
            continue; // timeout or error
        }

        PacketHeader* hdr = (PacketHeader*)buf;
        if (hdr->magic[0] != MAGIC[0] || hdr->magic[1] != MAGIC[1]) {
            continue;
        }

        if (hdr->type == PT_DISCOVERY_ANNOUNCE) {
            if (received < (int)(sizeof(PacketHeader) + sizeof(AnnouncePayload))) {
                continue;
            }

            AnnouncePayload* announce = (AnnouncePayload*)(buf + sizeof(PacketHeader));

            // Convert IP to string
            char ipStr[INET_ADDRSTRLEN];
            struct in_addr serverIp;
            serverIp.s_addr = announce->serverIp;
            inet_ntop(AF_INET, &serverIp, ipStr, sizeof(ipStr));

            // If we're not connected, or the server IP changed, send response
            {
                std::lock_guard<std::mutex> lock(m_driverMutex);
                if (!m_driverConnected || m_driverIP != ipStr) {
                    m_driverIP = ipStr;
                    m_driverAddr = {};
                    m_driverAddr.sin_family = AF_INET;
                    m_driverAddr.sin_port = htons(PORT_BROADCAST);
                    inet_pton(AF_INET, ipStr, &m_driverAddr.sin_addr);
                    m_driverConnected = true;

                    LOGD("Found driver: %s (%s)", announce->name, ipStr);
                }
            }

            // Send discovery response back to PC
            PacketHeader respHdr = {};
            respHdr.magic[0] = MAGIC[0];
            respHdr.magic[1] = MAGIC[1];
            respHdr.version = PROTOCOL_VERSION;
            respHdr.type = PT_DISCOVERY_RESPONSE;
            respHdr.payloadSize = sizeof(ResponsePayload);

            ResponsePayload respPayload = {};
            // Get our own IP - simplified, just send 0 for now (PC doesn't need it)
            respPayload.clientIp = 0;
            respPayload.clientPort = PORT_VIDEO;

            uint8_t respBuf[sizeof(PacketHeader) + sizeof(ResponsePayload)];
            memcpy(respBuf, &respHdr, sizeof(PacketHeader));
            memcpy(respBuf + sizeof(PacketHeader), &respPayload, sizeof(ResponsePayload));

            sendto(m_discoverySocket, respBuf, sizeof(respBuf), 0,
                   (sockaddr*)&fromAddr, sizeof(fromAddr));
        }
    }

    if (m_discoverySocket >= 0) {
        close(m_discoverySocket);
        m_discoverySocket = -1;
    }
    LOGD("Discovery loop ended");
}

// ============================================================
// Video: receive H264 chunks from driver
// ============================================================
void NetworkClient::VideoReceiveLoop() {
    LOGD("Video receive loop starting on port %d", PORT_VIDEO);

    m_videoSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_videoSocket < 0) {
        LOGE("Video socket() failed: %s", strerror(errno));
        return;
    }

    int reuse = 1;
    setsockopt(m_videoSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Large receive buffer for high-bandwidth video
    int rcvBuf = 4 * 1024 * 1024; // 4MB
    setsockopt(m_videoSocket, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(PORT_VIDEO);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_videoSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        LOGE("Video bind() failed: %s", strerror(errno));
        close(m_videoSocket);
        m_videoSocket = -1;
        return;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(m_videoSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LOGD("Video listening on port %d", PORT_VIDEO);

    uint8_t buf[65536 + sizeof(PacketHeader) + sizeof(VideoChunkHeader)];

    while (m_running) {
        int received = recvfrom(m_videoSocket, buf, sizeof(buf), 0, nullptr, nullptr);

        if (received < (int)sizeof(PacketHeader)) {
            continue;
        }

        PacketHeader* hdr = (PacketHeader*)buf;
        if (hdr->magic[0] != MAGIC[0] || hdr->magic[1] != MAGIC[1]) {
            continue;
        }

        if (hdr->type == PT_VIDEO_CHUNK &&
            received >= (int)(sizeof(PacketHeader) + sizeof(VideoChunkHeader))) {
            VideoChunkHeader* chunkHdr = (VideoChunkHeader*)(buf + sizeof(PacketHeader));
            const uint8_t* payload = buf + sizeof(PacketHeader) + sizeof(VideoChunkHeader);
            int payloadSize = received - sizeof(PacketHeader) - sizeof(VideoChunkHeader);

            if (m_videoChunkCallback && payloadSize > 0) {
                m_videoChunkCallback(payload, payloadSize, *chunkHdr);
            }
        }
    }

    if (m_videoSocket >= 0) {
        close(m_videoSocket);
        m_videoSocket = -1;
    }
    LOGD("Video receive loop ended");
}

// ============================================================
// Send tracking data to driver
// ============================================================
void NetworkClient::SendTracking(float oriW, float oriX, float oriY, float oriZ,
                                  float posX, float posY, float posZ) {
    if (!m_driverConnected) return;

    // Create tracking socket lazily
    if (m_trackingSocket < 0) {
        m_trackingSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_trackingSocket < 0) {
            LOGE("Tracking socket() failed: %s", strerror(errno));
            return;
        }
    }

    PacketHeader hdr = {};
    hdr.magic[0] = MAGIC[0];
    hdr.magic[1] = MAGIC[1];
    hdr.version = PROTOCOL_VERSION;
    hdr.type = PT_TRACKING;
    hdr.payloadSize = sizeof(TrackingPayload);

    TrackingPayload payload = {};
    payload.orientationW = oriW;
    payload.orientationX = oriX;
    payload.orientationY = oriY;
    payload.orientationZ = oriZ;
    payload.positionX = posX;
    payload.positionY = posY; // default height placeholder
    payload.positionZ = posZ;

    // Timestamp (nanoseconds)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    payload.timestampNanos = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    uint8_t buf[sizeof(PacketHeader) + sizeof(TrackingPayload)];
    memcpy(buf, &hdr, sizeof(PacketHeader));
    memcpy(buf + sizeof(PacketHeader), &payload, sizeof(TrackingPayload));

    sockaddr_in targetAddr = {};
    {
        std::lock_guard<std::mutex> lock(m_driverMutex);
        targetAddr = m_driverAddr;
        targetAddr.sin_port = htons(PORT_TRACKING);
    }

    sendto(m_trackingSocket, buf, sizeof(buf), 0,
           (sockaddr*)&targetAddr, sizeof(targetAddr));
}

// ============================================================
// Camera: send camera chunks to driver (placeholder)
// ============================================================
void NetworkClient::SendCameraChunk(const uint8_t* data, uint32_t size,
                                     uint32_t frameId, uint32_t chunkIndex,
                                     uint32_t totalChunks, uint32_t width, uint32_t height) {
    if (!m_driverConnected) return;

    // Create camera socket lazily
    if (m_cameraSocket < 0) {
        m_cameraSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_cameraSocket < 0) {
            LOGE("Camera socket() failed: %s", strerror(errno));
            return;
        }
    }

    PacketHeader hdr = {};
    hdr.magic[0] = MAGIC[0];
    hdr.magic[1] = MAGIC[1];
    hdr.version = PROTOCOL_VERSION;
    hdr.type = PT_CAMERA_CHUNK;
    hdr.payloadSize = sizeof(CameraChunkHeader) + size;

    CameraChunkHeader chunkHdr = {};
    chunkHdr.frameId = frameId;
    chunkHdr.chunkIndex = chunkIndex;
    chunkHdr.totalChunks = totalChunks;
    chunkHdr.frameSize = size;
    chunkHdr.width = width;
    chunkHdr.height = height;

    uint8_t buf[sizeof(PacketHeader) + sizeof(CameraChunkHeader) + CAMERA_CHUNK_SIZE];
    memcpy(buf, &hdr, sizeof(PacketHeader));
    memcpy(buf + sizeof(PacketHeader), &chunkHdr, sizeof(CameraChunkHeader));
    memcpy(buf + sizeof(PacketHeader) + sizeof(CameraChunkHeader), data, size);

    sockaddr_in targetAddr = {};
    {
        std::lock_guard<std::mutex> lock(m_driverMutex);
        targetAddr = m_driverAddr;
        targetAddr.sin_port = htons(PORT_CAMERA);
    }

    sendto(m_cameraSocket, buf, sizeof(PacketHeader) + sizeof(CameraChunkHeader) + size, 0,
           (sockaddr*)&targetAddr, sizeof(targetAddr));
}

}  // namespace cbpp
