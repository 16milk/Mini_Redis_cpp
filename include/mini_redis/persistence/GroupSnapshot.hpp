#pragma once

#include "mini_redis/cluster/SlotMigration.hpp"
#include "mini_redis/cluster/SlotOwnership.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/core/Expiration.hpp"

#include <string>

namespace persistence {

// Payload stored in consensus::Snapshot::data for a data Raft group. It carries
// logical objects, absolute TTLs, slot ownership/epoch and migration state.
// Raft term/index/membership live in the snapshot manifest, not here.
std::string encodeDataGroupSnapshot(
    const Database& database, const cluster::SlotOwnershipTable& ownership,
    const cluster::SlotMigrationStateMachine* migration = nullptr);

bool decodeDataGroupSnapshot(const std::string& bytes, Database& database,
                             cluster::SlotOwnershipTable& ownership,
                             cluster::SlotMigrationStateMachine* migration,
                             std::string& error);

// Convert a Redis RDB file into data-group snapshot bytes. This is an import
// path, not crash recovery: the resulting payload still has to be installed as
// a Raft snapshot (with identity, term, index and membership supplied by the
// caller) before the group can serve.
bool snapshotDataFromRdbFile(const std::string& rdb_path, UnixMillis now_ms,
                             std::string& snapshot_data, std::string& error);

}  // namespace persistence
