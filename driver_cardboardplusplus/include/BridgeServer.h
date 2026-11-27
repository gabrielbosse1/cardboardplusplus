#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include "Platform.h"
#include "BridgeProtocol.h"
namespace cbpp {
// Region names for the telemetry mapping and the bridge-owned command mapping; matched by the bridge
// process. The POSIX names carry the leading slash shm_open requires.
extern const char* kRegionName;
extern const char* kCmdRegionName;
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
    cbpp::PlatformMapping mapping_ = cbpp::kInvalidMapping;
    uint8_t*        base_ = nullptr;
    size_t          region_size_ = 0;
    uint32_t        slot_size_ = SLOT_SIZE;
    uint32_t        slot_count_ = SLOT_COUNT;
    std::atomic<bool>   running_{ false };
    std::atomic_flag    write_lock_ = ATOMIC_FLAG_INIT;
    cbpp::PlatformMapping cmd_mapping_ = cbpp::kInvalidMapping;
    uint8_t*        cmd_base_ = nullptr;
    size_t          cmd_region_size_ = 0;
    uint32_t        cmd_slot_size_ = CMD_SLOT_SIZE;
    uint32_t        cmd_slot_count_ = CMD_SLOT_COUNT;
    uint64_t        cmd_cursor_ = 0;
    std::mutex        telemetryMutex_;
    PayloadTelemetry  lastTelemetry_{};
    bool              hasTelemetry_ = false;
};
}