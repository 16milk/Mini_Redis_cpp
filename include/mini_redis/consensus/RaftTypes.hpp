#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace consensus {

using NodeId = std::string;
using GroupId = std::uint64_t;
using Term = std::uint64_t;
using Index = std::uint64_t;
using ProposalId = std::uint64_t;
using ReadId = std::uint64_t;

// Data shard ids are 1-based. Group 0 is permanently reserved for the
// independent metadata control-plane quorum.
constexpr GroupId kMetadataGroupId = 0;

// Learner 是成员身份，不是选举角色。选举角色只有 follower / candidate / leader。
enum class RaftRole {
    kFollower,
    kCandidate,
    kLeader,
};

enum class EntryType : std::uint8_t {
    kCommand = 1,
    kNoOp = 2,
    kConfig = 3,
};

enum class MessageType {
    kAppendEntries,
    kAppendEntriesResponse,
    kRequestVote,
    kRequestVoteResponse,
    kPreVote,
    kPreVoteResponse,
    kTimeoutNow,
    kInstallSnapshot,
    kInstallSnapshotResponse,
};

struct Membership {
    std::vector<NodeId> voters;
    std::vector<NodeId> learners;

    bool empty() const { return voters.empty() && learners.empty(); }
};

// incoming 始终是“当前配置”。joint 期间 outgoing 保存 C_old。
// 联合多数派必须同时满足 old 与 new，不能把两边 voter 并成一个集合再取多数。
struct Configuration {
    Membership incoming;
    Membership outgoing;

    bool joint() const { return !outgoing.voters.empty(); }
};

struct LogEntry {
    Term term = 0;
    Index index = 0;
    EntryType type = EntryType::kCommand;
    std::string payload;
    Configuration config;
};

struct HardState {
    Term current_term = 0;
    NodeId voted_for;
    Index commit_index = 0;
};

struct Snapshot {
    Index last_included_index = 0;
    Term last_included_term = 0;
    Configuration conf;
    std::string data;
};

struct Message {
    MessageType type = MessageType::kAppendEntries;
    NodeId from;
    NodeId to;
    GroupId group = 0;
    Term term = 0;

    Index prev_log_index = 0;
    Term prev_log_term = 0;
    Index leader_commit = 0;
    std::vector<LogEntry> entries;

    bool reject = false;
    Index match_index = 0;
    Index reject_hint_index = 0;
    Term reject_hint_term = 0;

    Index last_log_index = 0;
    Term last_log_term = 0;

    Snapshot snapshot;
    std::uint64_t seq = 0;
};

struct RaftOptions {
    std::int64_t heartbeat_interval_ms = 50;
    std::int64_t election_timeout_min_ms = 150;
    std::int64_t election_timeout_max_ms = 300;
    std::int64_t transfer_timeout_ms = 300;
    std::size_t max_entries_per_append = 64;
    Index learner_promote_max_lag = 1024;
    bool pre_vote = true;
    bool check_quorum = true;
};

struct RaftRestore {
    HardState hard_state;
    Configuration conf;
    Configuration snapshot_conf;
    Index snapshot_index = 0;
    Term snapshot_term = 0;
    std::string snapshot_data;
    std::vector<LogEntry> entries;
};

enum class ProposeError {
    kOk,
    kNotLeader,
    kTransferring,
    kBusyConfig,
    kNotVoter,
    kUnknownPeer,
    kLearnerLagging,
    kRemovingLeader,
};

struct ProposeResult {
    ProposeError error = ProposeError::kOk;
    ProposalId id = 0;
    Index index = 0;
    NodeId leader_hint;
};

enum class ReadError {
    kOk,
    kNotLeader,
    kTransferring,
    kNotVoter,
};

struct ReadRequest {
    ReadError error = ReadError::kOk;
    ReadId id = 0;
    Index read_index = 0;
    NodeId leader_hint;
};

struct ProposalOutcome {
    ProposalId id = 0;
    Index index = 0;
    bool committed = false;
};

struct ReadyRead {
    ReadId id = 0;
    Index read_index = 0;
};

struct Ready {
    std::optional<HardState> hard_state;
    std::optional<Index> truncate_from;
    std::vector<LogEntry> entries;
    std::optional<Snapshot> snapshot;
    std::vector<Message> messages;
    std::vector<LogEntry> committed;
    std::vector<ProposalOutcome> proposals;
    std::vector<ReadyRead> reads;

    bool persistNeeded() const {
        return hard_state.has_value() || truncate_from.has_value() || !entries.empty() ||
               snapshot.has_value();
    }

    bool empty() const {
        return !persistNeeded() && messages.empty() && committed.empty() &&
               proposals.empty() && reads.empty();
    }
};

bool isVoter(const Configuration& conf, const NodeId& id);
bool isLearner(const Configuration& conf, const NodeId& id);
bool isMember(const Configuration& conf, const NodeId& id);
std::vector<NodeId> allPeers(const Configuration& conf, const NodeId& self);
std::vector<NodeId> allVoters(const Configuration& conf);

Configuration normalizeConf(Configuration conf);
Configuration jointConf(const Membership& old_membership, const Membership& new_membership);
Configuration leaveJoint(const Configuration& conf);

bool hasMajority(const std::vector<NodeId>& voters,
                 const std::vector<NodeId>& acked);
bool quorumAcked(const Configuration& conf, const std::vector<NodeId>& acked);

} // namespace consensus
