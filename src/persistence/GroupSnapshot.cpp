#include "mini_redis/persistence/GroupSnapshot.hpp"

#include "mini_redis/persistence/Codec.hpp"
#include "mini_redis/persistence/LogicalObject.hpp"
#include "mini_redis/persistence/Rdb.hpp"

#include <unistd.h>
#include <utility>
#include <vector>

namespace persistence {
namespace {

constexpr char kPayloadMagic[] = {'M', 'R', 'D', 'P'};
constexpr std::uint16_t kPayloadVersion = 1;
constexpr std::size_t kMaxPayloadBytes = 1024U << 20;

}  // namespace

std::string encodeDataGroupSnapshot(const Database& database,
                                    const cluster::SlotOwnershipTable& ownership,
                                    const cluster::SlotMigrationStateMachine* migration) {
    const std::vector<StoredEntry> entries =
        database.exportKeys([](const std::string&) { return true; });
    ByteWriter writer;
    writer.magic(kPayloadMagic);
    writer.u16(kPayloadVersion);
    writer.u32(static_cast<std::uint32_t>(entries.size()));
    for (const StoredEntry& entry : entries) {
        writer.str(entry.key);
        writer.u64(static_cast<std::uint64_t>(entry.expire_at_ms));
        writer.str(entry.object ? encodeLogicalObject(*entry.object) : std::string());
    }
    writer.str(ownership.snapshotBytes());
    writer.str(migration != nullptr ? migration->snapshotBytes() : std::string());
    return writer.finish();
}

bool decodeDataGroupSnapshot(const std::string& bytes, Database& database,
                             cluster::SlotOwnershipTable& ownership,
                             cluster::SlotMigrationStateMachine* migration,
                             std::string& error) {
    if (bytes.size() > kMaxPayloadBytes) {
        error = "data group snapshot exceeds size limit";
        return false;
    }
    ByteReader reader(bytes);
    std::uint16_t version = 0;
    std::uint32_t count = 0;
    if (!reader.magic(kPayloadMagic) || !reader.u16(version) || version != kPayloadVersion ||
        !reader.u32(count) || count > kMaxCodecItems) {
        error = "invalid data group snapshot header";
        return false;
    }

    struct Item {
        std::string key;
        UnixMillis expire_at_ms = 0;
        std::shared_ptr<RedisObject> object;
    };
    std::vector<Item> items;
    items.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        Item item;
        std::uint64_t deadline = 0;
        std::string encoded;
        if (!reader.str(item.key) || !reader.u64(deadline) || !reader.str(encoded)) {
            error = "truncated data group snapshot object";
            return false;
        }
        item.expire_at_ms = static_cast<UnixMillis>(deadline);
        item.object = decodeLogicalObject(encoded, error);
        if (!item.object) {
            return false;
        }
        items.push_back(std::move(item));
    }

    std::string ownership_bytes;
    std::string migration_bytes;
    if (!reader.str(ownership_bytes, kMaxPayloadBytes) ||
        !reader.str(migration_bytes, kMaxPayloadBytes) || !reader.done()) {
        error = "truncated or trailing data group snapshot metadata";
        return false;
    }
    if (!ownership.installSnapshot(ownership_bytes, error)) {
        return false;
    }
    if (!migration_bytes.empty()) {
        if (migration == nullptr) {
            error = "data group snapshot has migration state but no state machine";
            return false;
        }
        if (!migration->installSnapshot(migration_bytes, error)) {
            return false;
        }
    }

    database.dropKeys([](const std::string&) { return true; });
    for (Item& item : items) {
        database.importKey(item.key, std::move(item.object), item.expire_at_ms);
    }
    error.clear();
    return true;
}

bool snapshotDataFromRdbFile(const std::string& rdb_path, UnixMillis now_ms,
                             std::string& snapshot_data, std::string& error) {
    if (access(rdb_path.c_str(), F_OK) == -1) {
        error = "RDB file '" + rdb_path + "' does not exist";
        return false;
    }
    const RdbLoadResult loaded = RdbEncoder::loadFromFile(rdb_path, now_ms);
    if (!RdbEncoder::lastError().empty()) {
        error = RdbEncoder::lastError();
        return false;
    }
    Database database(true);
    for (const auto& [key, object] : loaded.objects) {
        UnixMillis deadline = 0;
        const auto expire = loaded.expires.find(key);
        if (expire != loaded.expires.end()) {
            deadline = expire->second;
        }
        database.importKey(key, object, deadline);
    }
    cluster::SlotOwnershipTable ownership;
    snapshot_data = encodeDataGroupSnapshot(database, ownership, nullptr);
    error.clear();
    return true;
}

}  // namespace persistence
