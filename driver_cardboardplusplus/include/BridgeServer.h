// BridgeServer.h
// Producer side of the shared-memory bridge.
//
// Owns the named region and writes messages into the latest-wins ring. One
// instance lives inside HmdDriver and lives as long as the driver does.
//
// Thread-safety: Publish*() are called from the driver's Present/RunFrame (and
// later encoder) thread(s). We guard the ring base with a lightweight spinlock
// so a single reflected payload (e.g. telemetry from a teled thread) cannot
// race with frame submission. The on-wire layout itself stays lock-free for the
// consumer; the spinlock only serializes producers on the driver side.

#pragma once
#include <windows.h>
#include <cstdint>
#include <atomic>
#include "BridgeProtocol.h"

namespace cbpp {

// Published for logging; defined in BridgeServer.cpp.
extern const wchar_t* kRegionName;
// Command region (bridge -> driver settings); defined in BridgeServer.cpp.
extern const wchar_t* kCmdRegionName;

class BridgeServer {
public:
    BridgeServer();
    ~BridgeServer();

    // Creates the named region (idempotent). Returns false on failure.
    bool Start(uint32_t slotCount = SLOT_COUNT, uint32_t slotSize = SLOT_SIZE);

    // Releases the mapping and handle. Safe to call multiple times.
    void Stop();

    bool running() const { return running_; }

    // ---- Publish fixed-size payloads (status region; driver -> bridge) ----

    // Publish the given message type + payload bytes. Returns the sequence
    // number written, or 0 if the server is not running.
    uint64_t Publish(uint32_t msgType, const void* payload, uint32_t payloadLen);

    uint64_t PublishPose(const PayloadPose& p) { return Publish(MT_POSE, &p, sizeof(p)); }
    uint64_t PublishTelemetry(const PayloadTelemetry& t) { return Publish(MT_TELEMETRY, &t, sizeof(t)); }
    uint64_t PublishFrameSubmitted(const PayloadFrameSubmitted& f) { return Publish(MT_FRAME_SUBMITTED, &f, sizeof(f)); }
    uint64_t PublishTextureSetCreated(const PayloadTextureSetCreated& t) { return Publish(MT_TEXTURE_SET_CREATED, &t, sizeof(t)); }
    uint64_t PublishControllerInput(const PayloadControllerInput& c) { return Publish(MT_CONTROLLER_INPUT, &c, sizeof(c)); }
    uint64_t PublishCapReported(const PayloadCapReported& c) { return Publish(MT_CAP_REPORTED, &c, sizeof(c)); }

    // Convenience status telemetry: publishes a TELEMETRY message from a struct.
    void PublishStatus();

    // ---- Command region (bridge -> driver settings) ----

    // Opens the command region lazily (idempotent). If the bridge is not up yet
    // it simply leaves the mapping null and returns false; callers retry on the
    // next poll. Safe to call repeatedly.
    bool EnsureCmdMapping();

    // Polls the command region for a new SETTINGS message. Returns true once per
    // new settings change and fills `out`; returns false when idle. Never blocks.
    bool PollSettings(PayloadSettingsChange& out);

    // Releases the command mapping. Safe to call multiple times.
    void ShutdownCmdConsumer();

private:
    void* SlotPtr(uint64_t seq) const;

    bool OpenCmdMapping();
    void CleanupCmdMapping();
    void* CmdSlotPtr(uint64_t seq) const;
    bool CmdSlotValid(uint64_t seq) const;

    HANDLE          mapping_ = nullptr;
    uint8_t*        base_ = nullptr;
    uint32_t        slot_size_ = SLOT_SIZE;
    uint32_t        slot_count_ = SLOT_COUNT;
    std::atomic<bool>   running_{ false };
    // Serializes producer writes on the driver side.
    std::atomic_flag    write_lock_ = ATOMIC_FLAG_INIT;

    // Command region (bridge -> driver). The driver is the single consumer.
    HANDLE          cmd_mapping_ = nullptr;
    uint8_t*        cmd_base_ = nullptr;
    uint32_t        cmd_slot_size_ = CMD_SLOT_SIZE;
    uint32_t        cmd_slot_count_ = CMD_SLOT_COUNT;
    uint64_t        cmd_cursor_ = 0;
};

} // namespace cbpp