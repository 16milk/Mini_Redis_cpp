#pragma once

#include "mini_redis/cluster/MigrationAuth.hpp"
#include "mini_redis/cluster/SlotOwnership.hpp"
#include "mini_redis/cluster/Topology.hpp"
#include "mini_redis/consensus/RaftTypes.hpp"
#include "mini_redis/core/Expiration.hpp"

#include <cstddef>
#include <cstdint>
#include <climits>
#include <map>
#include <set>
#include <string>
#include <vector>

class Database;

namespace cluster {

constexpr std::uint16_t kSlotMigrationVersion = 1;

// One below the maximum, so that a migration always has room to advance the
// ownership epoch by one without wrapping.
constexpr std::uint64_t kMaxOwnershipEpoch = UINT64_MAX;

// The post-image of one key, not the command that produced it. Shipping effects
// instead of commands is what lets the target apply a batch twice, or apply it
// after a leader change, without reproducing source-side command semantics.
struct SlotDataRecord {
    std::uint64_t sequence = 0;   // 0 for base snapshot records, >0 for deltas
    std::string key;
    bool present = false;         // false is a tombstone
    std::string value;            // canonical value encoding, empty when absent
    UnixMillis expire_at_ms = 0;  // absolute deadline, 0 when there is no TTL
};

enum class MigrationLogOp : std::uint8_t {
    kSourceBegin = 1,     // source pins the base snapshot at this applied index
    kTargetBegin = 2,     // target opens the staging space
    kStageBase = 3,       // base snapshot chunk into staging
    kStageDelta = 4,      // contiguous post-snapshot deltas into staging
    kSourceFence = 5,     // source stops accepting writes at the old epoch, forever
    kTargetActivate = 6,  // staging becomes live data under the new epoch
    kSourceRelease = 7,   // source drops the slot and stops answering for it
    kTargetStable = 8,    // target leaves ASK-only mode once metadata committed
    kAbort = 9,           // only legal before the fence
    kForget = 10,         // drop a terminal record
};

// Evidence that the source shard's fence entry is committed, and that a leader
// which had proven its leadership with a read barrier observed it. Without the
// read barrier a deposed leader could sign a fence that its successor never saw.
struct MigrationCommitProof {
    std::string migration_id;
    SlotId slot = 0;
    ShardId source = kNoShard;
    ShardId target = kNoShard;
    std::uint64_t from_epoch = 0;
    std::uint64_t to_epoch = 0;
    consensus::Term fence_term = 0;
    consensus::Index fence_index = 0;
    consensus::Index read_index = 0;   // linearizable barrier, >= fence_index
    std::uint64_t fence_sequence = 0;  // last delta the source will ever emit
    UnixMillis logical_time_ms = 0;    // pins expiration while digests are compared
    std::string digest;
};

// The encoding carries a trailing HMAC tag, and decoding refuses anything that
// does not verify under the cluster key, so holding a decoded proof already
// means the bytes came from a cluster member.
std::string encodeCommitProof(const MigrationCommitProof& proof,
                              const MigrationKey& key);
bool decodeCommitProof(const std::string& encoded, const MigrationKey& key,
                       MigrationCommitProof& proof, std::string& error);
// Checks the proof is internally coherent. Decoding applies this too; it says
// nothing about whether the proof belongs to a particular migration, so callers
// still compare the identity fields against the record they are advancing.
bool validateCommitProof(const MigrationCommitProof& proof, std::string& error);

// The mirror image of the fence proof, produced by the target once activation
// is committed. The source demands one before it deletes its own copy, so
// neither side ever destroys the last replica of a slot on its own authority.
struct MigrationActivationProof {
    std::string migration_id;
    SlotId slot = 0;
    ShardId source = kNoShard;
    ShardId target = kNoShard;
    std::uint64_t from_epoch = 0;
    std::uint64_t to_epoch = 0;
    consensus::Term activate_term = 0;
    consensus::Index activate_index = 0;
    consensus::Index read_index = 0;  // linearizable barrier, >= activate_index
    std::string digest;               // must equal the source's fence digest
};

std::string encodeActivationProof(const MigrationActivationProof& proof,
                                  const MigrationKey& key);
bool decodeActivationProof(const std::string& encoded, const MigrationKey& key,
                           MigrationActivationProof& proof, std::string& error);
bool validateActivationProof(const MigrationActivationProof& proof,
                             std::string& error);

// 128-bit FNV-1a over the slot content, sorted by key and filtered at
// `logical_time_ms`. Source and target hold the same keys in different
// containers, so the digest has to be independent of both iteration order and
// of which side has already reaped an expired key.
std::string slotDigest(const std::vector<SlotDataRecord>& records,
                       UnixMillis logical_time_ms);

// Live content of one slot, in the shape the digest and the wire format use.
std::vector<SlotDataRecord> exportSlotRecords(const Database& database, SlotId slot,
                                              UnixMillis logical_time_ms);

struct SlotMigrationCommand {
    std::uint16_t version = kSlotMigrationVersion;
    MigrationLogOp op = MigrationLogOp::kSourceBegin;
    std::string migration_id;
    SlotId slot = 0;
    ShardId source = kNoShard;
    ShardId target = kNoShard;
    std::uint64_t from_epoch = 0;
    std::uint64_t to_epoch = 0;
    UnixMillis logical_time_ms = 0;
    bool base_complete = false;           // kStageBase: last chunk
    std::string commit_proof;             // kTargetActivate: the source's fence proof
    std::string activation_proof;         // kSourceRelease: the target's proof
    std::vector<SlotDataRecord> records;  // kStageBase / kStageDelta
};

std::string encodeSlotMigrationCommand(const SlotMigrationCommand& command);
bool decodeSlotMigrationCommand(const std::string& encoded,
                                SlotMigrationCommand& command, std::string& error);
// Migration commands and canonical writes share one data Raft log, so the
// applier tells them apart by their magic before decoding.
bool isSlotMigrationPayload(const std::string& payload);

enum class LocalMigrationRole : std::uint8_t {
    kNone = 0,
    kSource = 1,
    kTarget = 2,
};

enum class LocalMigrationStage : std::uint8_t {
    kSourceCopying = 1,
    kSourceFenced = 2,
    kSourceReleased = 3,
    kTargetStaging = 4,
    kTargetActive = 5,
    kTargetStable = 6,
    kAborted = 7,
};

// Everything a shard knows about one migration it participates in. This lives
// inside the data Raft state machine, so a restart or a leader change resumes
// from the last committed stage rather than from a coordinator's memory.
struct LocalMigrationState {
    std::string id;
    SlotId slot = 0;
    ShardId source = kNoShard;
    ShardId target = kNoShard;
    std::uint64_t from_epoch = 0;
    std::uint64_t to_epoch = 0;
    LocalMigrationRole role = LocalMigrationRole::kNone;
    LocalMigrationStage stage = LocalMigrationStage::kSourceCopying;

    // Source side.
    consensus::Index base_index = 0;  // applied index the base snapshot pins
    consensus::Index fence_index = 0;
    consensus::Term fence_term = 0;
    std::uint64_t next_sequence = 1;
    std::uint64_t fence_sequence = 0;
    UnixMillis fence_logical_time_ms = 0;
    std::string fence_digest;
    std::map<std::string, SlotDataRecord> base;
    std::map<std::uint64_t, SlotDataRecord> deltas;
    // Keys the journal has already spoken about. Derived from `deltas`, not
    // snapshotted, rebuilt on install. It exists so a delete of a key the
    // target has never heard of can be dropped instead of journaled: such a
    // tombstone changes nothing on the target, and without this a loop of
    // deletes against absent keys would grow the journal without bound.
    std::set<std::string> journaled_keys;

    // Target side. `staging` is not reachable from any client read path.
    bool base_complete = false;
    std::uint64_t staged_sequence = 0;
    consensus::Index activate_index = 0;
    consensus::Term activate_term = 0;
    std::map<std::string, SlotDataRecord> staging;
};

enum class MigrationApplyStatus : std::uint8_t {
    kApplied = 1,
    kDuplicate = 2,  // already applied; replaying it changed nothing
    kInvalidCommand = 3,
    kUnknownMigration = 4,
    kIllegalTransition = 5,
    kStaleEpoch = 6,
    kSequenceGap = 7,
    kDigestMismatch = 8,
    kProofRejected = 9,
};

struct MigrationApplyResult {
    MigrationApplyStatus status = MigrationApplyStatus::kInvalidCommand;
    std::string error;
    std::uint64_t sequence = 0;  // staged or fence sequence after the apply
    std::size_t records = 0;
};

// The migration half of a data shard's state machine. It owns the staging
// space, the delta journal and every transition of the local slot ownership
// record, so the whole protocol survives restarts as committed Raft state.
class SlotMigrationStateMachine {
public:
    SlotMigrationStateMachine(Database& database, SlotOwnershipTable& ownership,
                              ShardId shard, MigrationKey key)
        : database_(database),
          ownership_(ownership),
          shard_(shard),
          key_(std::move(key)) {}

    ShardId shard() const { return shard_; }
    // Coordinators sign the proofs they ship with the same key the verifying
    // shard will check them against.
    const MigrationKey& key() const { return key_; }

    MigrationApplyResult apply(const consensus::LogEntry& entry);
    MigrationApplyResult apply(const SlotMigrationCommand& command,
                               consensus::Index log_index, consensus::Term log_term);

    // Called by the data state machine after each committed write it applies.
    // Every replica runs it at the same point in the log, so the delta journal
    // is itself replicated state rather than a leader-local side effect.
    void captureWrite(SlotId slot, const std::vector<std::string>& keys,
                      UnixMillis logical_time_ms);

    const LocalMigrationState* find(const std::string& migration_id) const;
    const LocalMigrationState* forSlot(SlotId slot) const;
    const std::map<std::string, LocalMigrationState>& migrations() const {
        return migrations_;
    }

    // Source-side shipping. Both read committed state only, so a new leader
    // resumes shipping exactly where the previous one stopped. The journal is
    // retained in full until the slot is released or the migration is
    // abandoned: the source cannot verify how far the target has actually got,
    // and discarding a delta the target still needs would strand the slot with
    // no legal transition left.
    std::vector<SlotDataRecord> baseChunkAfter(const std::string& migration_id,
                                               const std::string& after_key,
                                               std::size_t max_records,
                                               bool& complete) const;
    std::vector<SlotDataRecord> deltasAfter(const std::string& migration_id,
                                            std::uint64_t after_sequence,
                                            std::size_t max_records) const;
    // `read_index` must come from a completed RaftCore::readIndex() on the
    // current source leader; it is what makes the proof unforgeable by a
    // deposed leader.
    bool buildCommitProof(const std::string& migration_id,
                          consensus::Index read_index, MigrationCommitProof& proof,
                          std::string& error) const;
    // Target-side counterpart, built once activation is committed. `read_index`
    // must come from a completed readIndex() on the current target leader.
    bool buildActivationProof(const std::string& migration_id,
                              consensus::Index read_index,
                              MigrationActivationProof& proof,
                              std::string& error) const;

    std::string snapshotBytes() const;
    bool installSnapshot(const std::string& bytes, std::string& error);

private:
    MigrationApplyResult applySourceBegin(const SlotMigrationCommand& command,
                                          consensus::Index log_index);
    MigrationApplyResult applyTargetBegin(const SlotMigrationCommand& command);
    MigrationApplyResult applyStageBase(const SlotMigrationCommand& command,
                                        LocalMigrationState& state);
    MigrationApplyResult applyStageDelta(const SlotMigrationCommand& command,
                                         LocalMigrationState& state);
    MigrationApplyResult applySourceFence(const SlotMigrationCommand& command,
                                          LocalMigrationState& state,
                                          consensus::Index log_index,
                                          consensus::Term log_term);
    MigrationApplyResult applyTargetActivate(const SlotMigrationCommand& command,
                                             LocalMigrationState& state,
                                             consensus::Index log_index,
                                             consensus::Term log_term);
    MigrationApplyResult applySourceRelease(const SlotMigrationCommand& command,
                                            LocalMigrationState& state);
    MigrationApplyResult applyTargetStable(LocalMigrationState& state);
    MigrationApplyResult applyAbort(LocalMigrationState& state);

    void rebuildSourceIndex();

    Database& database_;
    SlotOwnershipTable& ownership_;
    ShardId shard_;
    MigrationKey key_;
    std::map<std::string, LocalMigrationState> migrations_;
    // Slots this shard is currently the copying source for. Only these need a
    // delta record on every write.
    std::map<SlotId, std::string> capturing_slots_;
};

} // namespace cluster
