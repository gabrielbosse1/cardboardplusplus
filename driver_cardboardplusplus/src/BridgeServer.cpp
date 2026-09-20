#include "BridgeServer.h"
#include <cstring>
#include <mutex>
namespace cbpp {
const wchar_t* cbpp::kRegionName = L"Local\\cardboard_pp_bridge";
const wchar_t* cbpp::kCmdRegionName = L"Local\\cardboard_pp_bridge_cmd";
// Telemetry region name; the bridge opens the same name to read slots published here.
// Command region name; owned and written by the bridge, polled here for settings changes.
// Constructs an idle server; call Start to create the shared-memory region.
BridgeServer::BridgeServer() = default;
// Tears down both mappings; called from HmdDriver::ShutdownBridge via Stop/ShutdownCmdConsumer.
BridgeServer::~BridgeServer()
{
    Stop();
    ShutdownCmdConsumer();
}
// Creates and zeroes the telemetry file mapping with slotCount slots of slotSize bytes; called once from InitializeBridge.
bool BridgeServer::Start(uint32_t slotCount, uint32_t slotSize)
{
    if (running_.load(std::memory_order_acquire))
        return true;
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
    RegionHeader* h = reinterpret_cast<RegionHeader*>(base_);
    std::memset(h, 0, sizeof(RegionHeader));
    std::memcpy(h->magic, MAGIC, sizeof(MAGIC));
    h->version = VERSION;
    h->header_size = HEADER_SIZE;
    h->slot_size = slotSize;
    h->slot_count = slotCount;
    h->flags = 0;
    std::memset(base_ + HEADER_SIZE, 0, regionSize - HEADER_SIZE);
    StoreReleaseU64(reinterpret_cast<volatile uint64_t&>(h->write_seq), 0);
    _ReadWriteBarrier();
    running_.store(true, std::memory_order_release);
    return true;
}
// Unmaps and closes the telemetry region; leaves the command consumer to ShutdownCmdConsumer.
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
// Maps a sequence number to its ring slot; helper for Publish.
void* BridgeServer::SlotPtr(uint64_t seq) const
{
    const uint64_t idx = seq % slot_count_;
    return base_ + HEADER_SIZE + (size_t)idx * slot_size_;
}
// Releases the bridge-owned command mapping without touching the telemetry region.
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
// Opens the bridge-owned command region and validates magic/version/geometry; positions the cursor at the latest slot.
bool BridgeServer::OpenCmdMapping()
{
    const uint32_t regionSize = CMD_REGION_SIZE;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kCmdRegionName);
    if (!h || h == INVALID_HANDLE_VALUE)
        return false;
    uint8_t* base = static_cast<uint8_t*>(MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, regionSize));
    if (!base) {
        CloseHandle(h);
        return false;
    }
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
    uint64_t ws = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(header->write_seq));
    cmd_cursor_ = ws > 0 ? ws - 1 : 0;
    return true;
}
// Lazily attaches to the command region on first PollSettings; no-op when already mapped.
bool BridgeServer::EnsureCmdMapping()
{
    if (cmd_base_)
        return true;
    return OpenCmdMapping();
}
// Detaches the command consumer and resets its cursor; called from ShutdownBridge and the destructor.
void BridgeServer::ShutdownCmdConsumer()
{
    CleanupCmdMapping();
    cmd_cursor_ = 0;
}
// Maps a command sequence number to its ring slot; helper for PollSettings.
// Checks that a command slot holds the expected sequence and a non-empty message.
void* BridgeServer::CmdSlotPtr(uint64_t seq) const
{
    const uint64_t idx = seq % cmd_slot_count_;
    return cmd_base_ + HEADER_SIZE + (size_t)idx * cmd_slot_size_;
}
// Checks that a command slot holds the expected sequence and a non-empty message.
bool BridgeServer::CmdSlotValid(uint64_t seq) const
{
    SlotHeader* slot = reinterpret_cast<SlotHeader*>(CmdSlotPtr(seq));
    return slot->slot_seq == seq && slot->msg_type != MT_EMPTY;
}
// Drains new command slots on the RunFrame thread; copies MT_SETTINGS payloads into out for ApplyStreamSettings.
bool BridgeServer::PollSettings(PayloadSettingsChange& out)
{
    if (!EnsureCmdMapping())
        return false;
    RegionHeader* header = reinterpret_cast<RegionHeader*>(cmd_base_);
    uint64_t ws = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(header->write_seq));
    if (ws == cmd_cursor_)
        return false;
    if (ws < cmd_cursor_)
        cmd_cursor_ = 0;
    if (!CmdSlotValid(cmd_cursor_)) {
        cmd_cursor_ = ws > 0 ? ws - 1 : 0;
        if (!CmdSlotValid(cmd_cursor_)) {
            cmd_cursor_ = ws;
            return false;
        }
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
// Writes one message into the next ring slot under the spinlock; payload is truncated to the slot capacity.
uint64_t BridgeServer::Publish(uint32_t msgType, const void* payload, uint32_t payloadLen)
{
    if (!running_.load(std::memory_order_acquire) || !base_)
        return 0;
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
        StoreReleaseU64(reinterpret_cast<volatile uint64_t&>(h->write_seq), seq + 1);
    } while (false);
    write_lock_.clear(std::memory_order_release);
    return seq + 1;
}
// Caches the sample for PublishStatus and publishes it as MT_TELEMETRY; invoked from the encoder telemetry callback.
uint64_t BridgeServer::PublishTelemetry(const PayloadTelemetry& t)
{
    {
        std::lock_guard<std::mutex> lock(telemetryMutex_);
        lastTelemetry_ = t;
        hasTelemetry_ = true;
    }
    return Publish(MT_TELEMETRY, &t, sizeof(t));
}
// Republishes the last telemetry sample with the live write_seq; driven by the 1 Hz RunBridgeHeartbeat.
void BridgeServer::PublishStatus()
{
    PayloadTelemetry t;
    {
        std::lock_guard<std::mutex> lock(telemetryMutex_);
        if (!hasTelemetry_)
            return;
        t = lastTelemetry_;
    }
    t.summary_frames = LoadRelaxedU64(reinterpret_cast<volatile uint64_t&>(
        reinterpret_cast<RegionHeader*>(base_)->write_seq));
    Publish(MT_TELEMETRY, &t, sizeof(t));
}
}