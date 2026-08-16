#include "BridgeConnection.h"
#include <cstring>
#include <sstream>

static const int BRIDGE_PORT = 42071;
static const char* BRIDGE_HOST = "127.0.0.1";

void DriverLog(const char* pFormat, ...);

BridgeConnection::BridgeConnection()
    : m_clientSocket(INVALID_SOCKET)
    , m_bridgeConnected(false)
    , m_streamAuthorized(false)
    , m_running(false)
    , m_phoneConnected(false)
    , m_phoneFps(0)
    , m_phoneFps1Low(0)
    , m_compositorFps(0.0f)
    , m_encoderFps(0.0f)
{
}

BridgeConnection::~BridgeConnection() {
    Shutdown();
}

bool BridgeConnection::Initialize() {
    DriverLog("BridgeConnection::Initialize");

    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        DriverLog("BridgeConnection: WSAStartup failed! Error: %d", result);
        return false;
    }

    m_running = true;
    m_connectionThread = std::thread(&BridgeConnection::ConnectionThreadFunc, this);

    DriverLog("BridgeConnection: Started connection thread");
    return true;
}

void BridgeConnection::Shutdown() {
    DriverLog("BridgeConnection::Shutdown");

    m_running = false;

    if (m_connectionThread.joinable()) {
        m_connectionThread.join();
    }

    if (m_clientSocket != INVALID_SOCKET) {
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
    }

    m_bridgeConnected = false;
    m_streamAuthorized = false;

    WSACleanup();
    DriverLog("BridgeConnection: Shutdown complete");
}

void BridgeConnection::SetPhoneConnected(bool connected) {
    m_phoneConnected = connected;
}

void BridgeConnection::SetPhoneFps(int fps) {
    m_phoneFps = fps;
}

void BridgeConnection::SetPhoneFps1Low(int fps1low) {
    m_phoneFps1Low = fps1low;
}

void BridgeConnection::SetCompositorFps(float fps) {
    m_compositorFps = fps;
}

void BridgeConnection::SetEncoderFps(float fps) {
    m_encoderFps = fps;
}

void BridgeConnection::ConnectionThreadFunc() {
    DriverLog("BridgeConnection: Connection thread started");

    while (m_running) {
        // Try to connect to bridge
        m_clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_clientSocket == INVALID_SOCKET) {
            DriverLog("BridgeConnection: socket() failed! Error: %d", WSAGetLastError());
            Sleep(5000);
            continue;
        }

        sockaddr_in bridgeAddr;
        bridgeAddr.sin_family = AF_INET;
        bridgeAddr.sin_port = htons(BRIDGE_PORT);
        inet_pton(AF_INET, BRIDGE_HOST, &bridgeAddr.sin_addr);

        if (connect(m_clientSocket, (sockaddr*)&bridgeAddr, sizeof(bridgeAddr)) == SOCKET_ERROR) {
            closesocket(m_clientSocket);
            m_clientSocket = INVALID_SOCKET;
            Sleep(3000);
            continue;
        }

        DriverLog("BridgeConnection: Connected to bridge at %s:%d", BRIDGE_HOST, BRIDGE_PORT);
        m_bridgeConnected = true;

        // Listen for commands from bridge
        char buffer[1024];
        std::string pendingData;

        while (m_running) {
            int bytesReceived = recv(m_clientSocket, buffer, sizeof(buffer) - 1, 0);
            if (bytesReceived <= 0) {
                if (bytesReceived == 0) {
                    DriverLog("BridgeConnection: Bridge closed connection");
                } else {
                    DriverLog("BridgeConnection: recv() failed! Error: %d", WSAGetLastError());
                }
                break;
            }

            buffer[bytesReceived] = '\0';
            pendingData += buffer;

            // Process complete lines
            size_t pos;
            while ((pos = pendingData.find('\n')) != std::string::npos) {
                std::string line = pendingData.substr(0, pos);
                pendingData.erase(0, pos + 1);

                if (!line.empty()) {
                    DriverLog("BridgeConnection: Received command: %s", line.c_str());
                    HandleCommand(line);
                }
            }
        }

        // Disconnected
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
        m_bridgeConnected = false;
        m_streamAuthorized = false;

        if (OnStreamAuthorizationChanged) {
            OnStreamAuthorizationChanged(false);
        }

        DriverLog("BridgeConnection: Disconnected from bridge, retrying...");
        Sleep(2000);
    }

    DriverLog("BridgeConnection: Connection thread exiting");
}

void BridgeConnection::HandleCommand(const std::string& cmd) {
    if (cmd == "START_STREAM") {
        DriverLog("BridgeConnection: Stream AUTHORIZED by bridge");
        m_streamAuthorized = true;
        if (OnStreamAuthorizationChanged) {
            OnStreamAuthorizationChanged(true);
        }
        SendLine("ACK");
    }
    else if (cmd == "STOP_STREAM") {
        DriverLog("BridgeConnection: Stream DENIED by bridge");
        m_streamAuthorized = false;
        if (OnStreamAuthorizationChanged) {
            OnStreamAuthorizationChanged(false);
        }
        SendLine("ACK");
    }
    else if (cmd == "GET_STATUS") {
        SendStatus();
    }
    else if (cmd.rfind("SET_PHONE_FPS ", 0) == 0) {
        int fps = 0;
        if (sscanf_s(cmd.c_str(), "SET_PHONE_FPS %d", &fps) == 1) {
            m_phoneFps = fps;
        }
    }
    else if (cmd.rfind("SET_PHONE_FPS1LOW ", 0) == 0) {
        int low = 0;
        if (sscanf_s(cmd.c_str(), "SET_PHONE_FPS1LOW %d", &low) == 1) {
            m_phoneFps1Low = low;
        }
    }
    else if (cmd.rfind("SET_PHONE_CONNECTED ", 0) == 0) {
        bool connected = (cmd.find("true") != std::string::npos);
        m_phoneConnected = connected;
    }
    else {
        DriverLog("BridgeConnection: Unknown command: %s", cmd.c_str());
    }
}

void BridgeConnection::SendStatus() {
    std::ostringstream oss;
    oss << "STATUS "
        << "compositor_fps=" << m_compositorFps.load() << " "
        << "encoder_fps=" << m_encoderFps.load() << " "
        << "phone_connected=" << (m_phoneConnected.load() ? "true" : "false") << " "
        << "phone_fps=" << m_phoneFps.load() << " "
        << "phone_fps_1low=" << m_phoneFps1Low.load();
    SendLine(oss.str());
}

void BridgeConnection::SendLine(const std::string& line) {
    if (m_clientSocket == INVALID_SOCKET) return;

    std::lock_guard<std::mutex> lock(m_sendMutex);

    std::string data = line + "\n";
    int sent = send(m_clientSocket, data.c_str(), (int)data.size(), 0);
    if (sent == SOCKET_ERROR) {
        DriverLog("BridgeConnection: send() failed! Error: %d", WSAGetLastError());
    }
}
