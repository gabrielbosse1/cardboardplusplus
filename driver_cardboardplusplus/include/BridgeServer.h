#pragma once
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include "BridgeProtocol.h"
namespace cbpp {
// Region names for the telemetry mapping and the bridge-owned command mapping; matched by the bridge process.
extern const wchar_t* kRegionName;
extern const wchar_t* kCmdRegionName;
// Driver-side shared-memory endpoint; publishes telemetry/status for the bridge and polls bridge settings.
class BridgeServer {
public:
// Lifetime + publish API; Start/Stop manage the mapping, Publish* write slots read by the bridge, PollSettings reads bridge commands.
    BridgeServer();
    ~BridgeServer();
    bool Start(uint32_t slotCount = SLOT_COUNT, uint32_t slotSize = SLOT_SIZE);
    void Stop();
    bool running() const { return running_; }
    uint64_t Publish(uint32_t msgType, const void* payload, uint32_t payloadLen);
    uint64_t PublishTelemetry(const PayloadTelemetry& t);
    void PublishStatus();
    bool EnsureCmdMapping();
    bool PollSettings(PayloadSettingsChange& out);
    void ShutdownCmdConsumer();
private:
// Slot arithmetic and command-mapping helpers; internal to Start/Publish/PollSettings.
    void* SlotPtr(uint64_t seq) const;
    bool OpenCmdMapping();
    void CleanupCmdMapping();
    void* CmdSlotPtr(uint64_t seq) const;
    bool CmdSlotValid(uint64_t seq) const;
// Handles, geometry, cursors, and cached telemetry; write_lock_ serializes Publish, telemetryMutex_ guards the last sample.
    HANDLE          mapping_ = nullptr;
    uint8_t*        base_ = nullptr;
    uint32_t        slot_size_ = SLOT_SIZE;
    uint32_t        slot_count_ = SLOT_COUNT;
    std::atomic<bool>   running_{ false };
    std::atomic_flag    write_lock_ = ATOMIC_FLAG_INIT;
    HANDLE          cmd_mapping_ = nullptr;
    uint8_t*        cmd_base_ = nullptr;
    uint32_t        cmd_slot_size_ = CMD_SLOT_SIZE;
    uint32_t        cmd_slot_count_ = CMD_SLOT_COUNT;
    uint64_t        cmd_cursor_ = 0;
    std::mutex        telemetryMutex_;
    PayloadTelemetry  lastTelemetry_{};
    bool              hasTelemetry_ = false;
};
}