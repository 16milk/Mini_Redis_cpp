#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace runtime {

// Isolated traffic classes. Heartbeats and votes must not share a queue,
// connection quota or buffer pool with snapshots, migrations or slow clients.
enum class TrafficClass : std::uint8_t {
    kClient = 0,
    kRaft = 1,
    kAdmin = 2,
    kMigration = 3,
    kSnapshot = 4,
};

constexpr std::size_t kTrafficClassCount = 5;

inline bool isControlTraffic(TrafficClass cls) {
    return cls == TrafficClass::kRaft || cls == TrafficClass::kAdmin;
}

inline bool isBulkTraffic(TrafficClass cls) {
    return cls == TrafficClass::kSnapshot || cls == TrafficClass::kMigration;
}

inline const char* trafficClassName(TrafficClass cls) {
    switch (cls) {
        case TrafficClass::kClient:
            return "client";
        case TrafficClass::kRaft:
            return "raft";
        case TrafficClass::kAdmin:
            return "admin";
        case TrafficClass::kMigration:
            return "migration";
        case TrafficClass::kSnapshot:
            return "snapshot";
    }
    return "unknown";
}

// Per-turn budgets and high/low watermarks. High pauses further reads; low
// resumes them so a slow downstream (Raft, WAL, or the client) pushes back
// into the socket instead of growing unbounded queues.
struct IoLimits {
    std::size_t read_bytes_per_turn = 64 * 1024;
    std::size_t read_calls_per_turn = 16;
    std::size_t commands_per_turn = 64;
    std::chrono::microseconds command_runtime_per_turn{1000};
    std::size_t write_bytes_per_turn = 64 * 1024;
    std::size_t write_calls_per_turn = 16;

    std::size_t input_high_watermark = 1024 * 1024;
    std::size_t input_low_watermark = 512 * 1024;
    std::size_t output_high_watermark = 4 * 1024 * 1024;
    std::size_t output_low_watermark = 2 * 1024 * 1024;

    // In-flight = submitted but not yet written as a consecutive response.
    std::size_t max_inflight = 512;
    std::size_t inflight_high_watermark = 128;
    std::size_t inflight_low_watermark = 64;

    std::size_t mailbox_high_watermark = 1024;
    std::size_t mailbox_low_watermark = 256;
    std::size_t mailbox_bytes_high_watermark = 32 * 1024 * 1024;
    std::size_t mailbox_bytes_low_watermark = 8 * 1024 * 1024;
    std::size_t wal_bytes_high_watermark = 64 * 1024 * 1024;
    std::size_t wal_bytes_low_watermark = 16 * 1024 * 1024;
};

inline IoLimits clientIoLimits() { return {}; }

inline IoLimits raftIoLimits() {
    IoLimits limits;
    limits.read_bytes_per_turn = 16 * 1024;
    limits.read_calls_per_turn = 8;
    limits.commands_per_turn = 32;
    limits.write_bytes_per_turn = 16 * 1024;
    limits.write_calls_per_turn = 8;
    limits.input_high_watermark = 256 * 1024;
    limits.input_low_watermark = 64 * 1024;
    limits.output_high_watermark = 256 * 1024;
    limits.output_low_watermark = 64 * 1024;
    limits.max_inflight = 1024;
    limits.inflight_high_watermark = 512;
    limits.inflight_low_watermark = 128;
    limits.mailbox_high_watermark = 8192;
    limits.mailbox_low_watermark = 1024;
    limits.mailbox_bytes_high_watermark = 8 * 1024 * 1024;
    limits.mailbox_bytes_low_watermark = 1 * 1024 * 1024;
    return limits;
}

inline IoLimits adminIoLimits() {
    IoLimits limits;
    limits.commands_per_turn = 16;
    limits.input_high_watermark = 256 * 1024;
    limits.input_low_watermark = 64 * 1024;
    limits.output_high_watermark = 1 * 1024 * 1024;
    limits.output_low_watermark = 256 * 1024;
    limits.max_inflight = 64;
    limits.inflight_high_watermark = 16;
    limits.inflight_low_watermark = 4;
    return limits;
}

inline IoLimits bulkIoLimits() {
    IoLimits limits;
    limits.read_bytes_per_turn = 256 * 1024;
    limits.read_calls_per_turn = 32;
    limits.commands_per_turn = 8;
    limits.command_runtime_per_turn = std::chrono::milliseconds(4);
    limits.write_bytes_per_turn = 256 * 1024;
    limits.write_calls_per_turn = 32;
    limits.input_high_watermark = 8 * 1024 * 1024;
    limits.input_low_watermark = 2 * 1024 * 1024;
    limits.output_high_watermark = 16 * 1024 * 1024;
    limits.output_low_watermark = 4 * 1024 * 1024;
    limits.max_inflight = 32;
    limits.inflight_high_watermark = 8;
    limits.inflight_low_watermark = 2;
    limits.mailbox_high_watermark = 256;
    limits.mailbox_low_watermark = 32;
    limits.mailbox_bytes_high_watermark = 64 * 1024 * 1024;
    limits.mailbox_bytes_low_watermark = 16 * 1024 * 1024;
    return limits;
}

inline IoLimits limitsFor(TrafficClass cls) {
    switch (cls) {
        case TrafficClass::kRaft:
            return raftIoLimits();
        case TrafficClass::kAdmin:
            return adminIoLimits();
        case TrafficClass::kSnapshot:
        case TrafficClass::kMigration:
            return bulkIoLimits();
        case TrafficClass::kClient:
            break;
    }
    return clientIoLimits();
}

struct ResourceQuota {
    std::size_t max_connections = 10000;
    std::size_t max_buffer_bytes = 256 * 1024 * 1024;
};

inline ResourceQuota quotaFor(TrafficClass cls) {
    ResourceQuota quota;
    switch (cls) {
        case TrafficClass::kRaft:
            quota.max_connections = 512;
            quota.max_buffer_bytes = 64 * 1024 * 1024;
            break;
        case TrafficClass::kAdmin:
            quota.max_connections = 32;
            quota.max_buffer_bytes = 16 * 1024 * 1024;
            break;
        case TrafficClass::kSnapshot:
        case TrafficClass::kMigration:
            quota.max_connections = 64;
            quota.max_buffer_bytes = 128 * 1024 * 1024;
            break;
        case TrafficClass::kClient:
            break;
    }
    return quota;
}

}  // namespace runtime
