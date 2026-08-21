// BridgeServer.cpp
#include "BridgeServer.h"
#include <cstring>
#include <mutex>

namespace cbpp {

// Must match bridge/crates/bridge-shm/src/mem.rs region_name().
const wchar_t* cbpp::kRegionName = L"Local\\cardboard_pp_bridge";
// Must match bridge/crates/bridge-shm/src/mem.rs cmd_region_name().
const wchar_t* cbpp::kCmdRegionName = L"Local\\cardboard_pp_bridge_cmd";

BridgeServer::BridgeServer() = default;

BridgeServer::~BridgeServer()
{
    Stop();
    ShutdownCmdConsumer();
}

bool BridgeServer::Start(uint32_t slotCount, uint32_t slotSize)
{
    if (running_.load(std::memory_order_acquire))
        return true; // already running

    if (slotSize < MIN_SLOT_SIZE || slotCount == 0)
        return false;

    const uint64_t regionSize = HEADER_SIZE + (uint64_t)slotSize * slotCount;

    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        (DWORD)(regionSize >> 32),
        (DWORD)(regionSize & 0xFFFFFFFFu),
        kRegionName);

    if (!mapping_ || mapping_ == INVALID_HANDLE_VALUE)
        return false;

    base_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!base_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    slot_size_ = slotSize;
    slot_count_ = slotCount;

    // Initialize header.
    RegionHeader* h = reinterpret_cast<RegionHeader*>(base_);
    std::memset(h, 0, sizeof(RegionHeader));
    std::memcpy(h->magic, MAGIC, sizeof(MAGIC));
    h->version = VERSION;
    h->header_size = HEADER_SIZE;
    h->slot_size = slotSize;
    h->slot_count = slotCount;
    h->flags = 0;

    // Wipe slots so consumers won't misread stale seq/magic.
    std::memset(base_ + HEADER_SIZE, 0, regionSize - HEADER_SIZE);

    // Publish the header now that it's consistent. Release store so consumers
    // that read magic/version see them only after the writes land.
    StoreReleaseU64(reinterpret_cast<volatile uint64_t&>(h->write_seq), 0);
    _ReadWriteBarrier();

    running_.store(true, std::memory_order_release);
    return true;
}

void BridgeServer::Stop()
{
    running_.store(false, std::memory_order_release);

    if (base_) {
        UnmapViewOfFile(base_);
        base_ = nullptr;
    }
    if (mapping_ && mapping_ != INVALID_HANDLE_VALUE) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

void* BridgeServer::SlotPtr(uint64_t seq) const
{
    const uint64_t idx = seq % slot_count_;
    return base_ + HEADER_SIZE + (size_t)idx * slot_size_;
}

// ---- Command region (bridge -> driver settings) ----

void BridgeServer::CleanupCmdMapping()
{
    if (cmd_base_) {
        UnmapViewOfFile(cmd_base_);
        cmd_base_ = nullptr;
    }
    if (cmd_mapping_ && cmd_mapping_ != INVALID_HANDLE_VALUE) {
        CloseHandle(cmd_mapping_);
        cmd_mapping_ = nullptr;
    }
}

bool BridgeServer::OpenCmdMapping()
{
    const uint32_t regionSize = CMD_REGION_SIZE;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kCmdRegionName);
    if (!h || h == INVALID_HANDLE_VALUE)
        return false; // bridge not up yet; caller retries later

    uint8_t* base = static_cast<uint8_t*>(MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, regionSize));
    if (!base) {
        CloseHandle(h);
        return false;
    }

    // Validate header the bridge produced.
    RegionHeader* header = reinterpret_cast<RegionHeader*>(base);
    if (std::memcmp(header->magic, MAGIC, sizeof(MAGIC)) != 0 ||
        header->version != VERSION || header->header_size != HEADER_SIZE ||
        header->slot_size < MIN_SLOT_SIZE || header->slot_count == 0) {
        UnmapViewOfFile(base);
        CloseHandle(h);
        return false;
    }

    cmd_mapping_ = h;
    cmd_base_ = base;
    cmd_slot_size_ = header->slot_size;
    cmd_slot_count_ = header->slot_count;
    // Settings are *state*, not events: if the bridge already pushed before we
    // mapped the region, rewind by one so the latest slot is re-applied on the
    // next poll (driver inherits the persisted bridge settings on start).
    uint64_t ws = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(header->write_seq));
    cmd_cursor_ = ws > 0 ? ws - 1 : 0;
    return true;
}

bool BridgeServer::EnsureCmdMapping()
{
    if (cmd_base_)
        return true;
    return OpenCmdMapping();
}

void BridgeServer::ShutdownCmdConsumer()
{
    CleanupCmdMapping();
    cmd_cursor_ = 0;
}

void* BridgeServer::CmdSlotPtr(uint64_t seq) const
{
    const uint64_t idx = seq % cmd_slot_count_;
    return cmd_base_ + HEADER_SIZE + (size_t)idx * cmd_slot_size_;
}

bool BridgeServer::CmdSlotValid(uint64_t seq) const
{
    SlotHeader* slot = reinterpret_cast<SlotHeader*>(CmdSlotPtr(seq));
    return slot->slot_seq == seq && slot->msg_type != MT_EMPTY;
}

bool BridgeServer::PollSettings(PayloadSettingsChange& out)
{
    if (!EnsureCmdMapping())
        return false;

    RegionHeader* header = reinterpret_cast<RegionHeader*>(cmd_base_);
    uint64_t ws = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(header->write_seq));
    if (ws == cmd_cursor_)
        return false;

    // A writer restart (bridge re-created a fresh region) can move write_seq
    // backwards; re-wind and re-validate rather than skipping forever.
    if (ws < cmd_cursor_)
        cmd_cursor_ = 0;

    if (!CmdSlotValid(cmd_cursor_)) {
        // Producer overwrote our slot before we read it; latest-wins.
        cmd_cursor_ = ws;
        return false;
    }

    SlotHeader* slot = reinterpret_cast<SlotHeader*>(CmdSlotPtr(cmd_cursor_));
    bool isSettings = (slot->msg_type == MT_SETTINGS);
    if (isSettings && slot->payload_len >= sizeof(PayloadSettingsChange)) {
        out = *reinterpret_cast<const PayloadSettingsChange*>(
            reinterpret_cast<uint8_t*>(slot) + sizeof(SlotHeader));
    }
    cmd_cursor_ = ws;
    return isSettings;
}

uint64_t BridgeServer::Publish(uint32_t msgType, const void* payload, uint32_t payloadLen)
{
    if (!running_.load(std::memory_order_acquire) || !base_)
        return 0;

    // Serialize producers on the driver side.
    while (write_lock_.test_and_set(std::memory_order_acquire))
        ;
    uint64_t seq = 0;
    do {
        RegionHeader* h = reinterpret_cast<RegionHeader*>(base_);
        seq = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(h->write_seq));

        SlotHeader* slot = reinterpret_cast<SlotHeader*>(SlotPtr(seq));
        std::memset(slot, 0, slot_size_);
        slot->slot_seq = seq;
        slot->msg_type = msgType;
        uint32_t cap = slot_size_ - sizeof(SlotHeader);
        uint32_t n = payloadLen;
        if (n > cap) n = cap;
        slot->payload_len = n;
        if (payload && n)
            std::memcpy(reinterpret_cast<uint8_t*>(slot) + sizeof(SlotHeader), payload, n);

        // Publish: header write_seq = seq + 1 becomes visible after the slot.
        StoreReleaseU64(reinterpret_cast<volatile uint64_t&>(h->write_seq), seq + 1);
    } while (false);

    write_lock_.clear(std::memory_order_release);
    return seq + 1;
}

void BridgeServer::PublishStatus()
{
    PayloadTelemetry t;
    std::memset(&t, 0, sizeof(t));
    // Reflect region health into telemetry so the UI can show liveness.
    t.summary_frames = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(
        reinterpret_cast<RegionHeader*>(base_)->write_seq));
    PublishTelemetry(t);
}

} // namespace cbpp