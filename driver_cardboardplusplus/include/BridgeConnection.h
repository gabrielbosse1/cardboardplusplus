#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <functional>

#pragma comment(lib, "ws2_32.lib")

class BridgeConnection {
public:
    BridgeConnection();
    ~BridgeConnection();

    bool Initialize();
    void Shutdown();

    bool IsBridgeConnected() const { return m_bridgeConnected.load(); }
    bool IsStreamAuthorized() const { return m_streamAuthorized.load(); }

    void SetPhoneConnected(bool connected);
    void SetPhoneFps(int fps);
    void SetPhoneFps1Low(int fps1low);
    void SetCompositorFps(float fps);
    void SetEncoderFps(float fps);

    std::function<void(bool)> OnStreamAuthorizationChanged;

private:
    void ConnectionThreadFunc();
    void HandleCommand(const std::string& cmd);
    void SendStatus();
    void SendLine(const std::string& line);

    SOCKET m_clientSocket;
    std::atomic<bool> m_bridgeConnected;
    std::atomic<bool> m_streamAuthorized;
    std::atomic<bool> m_running;
    std::thread m_connectionThread;

    std::mutex m_sendMutex;

    // Status to report to bridge
    std::atomic<bool> m_phoneConnected;
    std::atomic<int> m_phoneFps;
    std::atomic<int> m_phoneFps1Low;
    std::atomic<float> m_compositorFps;
    std::atomic<float> m_encoderFps;
};
