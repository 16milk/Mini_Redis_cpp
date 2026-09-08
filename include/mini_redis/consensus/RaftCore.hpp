#pragma once

#include "mini_redis/consensus/RaftLog.hpp"
#include "mini_redis/consensus/RaftTypes.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace command {
struct CanonicalCommand;
}

namespace consensus {

// 纯 Raft 状态机。输入是 tick / RPC / propose / durable completion / applied 回告，
// 输出是待持久化记录、待发送消息、已提交可 apply 的日志和已确认的 ReadIndex。
// 驱动必须先持久化 Ready 中的 hard state 与日志，再发送消息，再 apply；
// 这样 Follower 的成功响应和 Leader 的提交都发生在多数派耐久化之后。
class RaftCore {
public:
    RaftCore(NodeId self, GroupId group, RaftRestore restore, RaftOptions options = {});

    const NodeId& selfId() const { return self_; }
    GroupId groupId() const { return group_; }
    RaftRole role() const { return role_; }
    Term term() const { return current_term_; }
    Index commitIndex() const { return commit_index_; }
    Index appliedIndex() const { return applied_index_; }
    Index lastIndex() const { return log_.lastIndex(); }
    Index lastPersisted() const { return last_persisted_; }
    const NodeId& votedFor() const { return voted_for_; }
    const NodeId& leaderId() const { return leader_; }
    const Configuration& configuration() const { return conf_; }
    bool transferring() const { return transferring_; }
    bool committedCurrentTerm() const { return committed_current_term_; }
    bool isLocalVoter() const { return isVoter(conf_, self_); }
    bool isLocalLearner() const { return isLearner(conf_, self_); }

    Index matchIndex(const NodeId& peer) const;
    Index nextIndex(const NodeId& peer) const;

    void tick(std::int64_t now_ms);
    void step(Message msg);

    // Data commands should use this entry point so raw RESP frames and encoded
    // client responses can never become command-log payloads.
    ProposeResult proposeCanonical(const command::CanonicalCommand& command);
    ProposeResult propose(std::string payload);
    ProposeResult addLearner(const NodeId& node_id);
    ProposeResult promoteLearner(const NodeId& node_id);
    ProposeResult removeNode(const NodeId& node_id);
    ProposeResult demoteVoter(const NodeId& node_id);

    bool transferLeadership(const NodeId& target);
    ReadRequest readIndex();

    void reportApplied(Index index);
    bool compact(Index index, std::string snapshot_data);

    bool hasReady() const;
    Ready takeReady();
    void advance();

private:
    struct Progress {
        Index next = 1;
        Index match = 0;
        bool recent = false;
        bool snapshot_inflight = false;
        std::uint64_t acked_seq = 0;
    };

    struct PendingProposal {
        ProposalId id = 0;
        Index index = 0;
        Term term = 0;
    };

    struct PendingRead {
        ReadId id = 0;
        Index read_index = 0;
        std::uint64_t need_seq = 0;
        bool quorum_acked = false;
    };

    void becomeFollower(Term term, NodeId leader);
    void becomeCandidate();
    void becomeLeader();
    void campaign(bool pre_vote);
    void resetElectionTimer();
    void abortTransfer();

    void handleAppendEntries(const Message& msg);
    void handleAppendEntriesResponse(const Message& msg);
    void handleVoteRequest(const Message& msg, bool pre_vote);
    void handleVoteResponse(const Message& msg, bool pre_vote);
    void handleTimeoutNow(const Message& msg);
    void handleInstallSnapshot(const Message& msg);
    void handleInstallSnapshotResponse(const Message& msg);

    bool logUpToDate(Index last_index, Term last_term) const;
    bool canGrantVote(const NodeId& candidate) const;

    void appendEntry(LogEntry entry);
    ProposeResult proposeConfig(Configuration conf, std::string payload);
    void applyConfigFromEntry(const LogEntry& entry);
    void maybeCommit();
    void maybeLeaveJoint();
    void maybeSendTimeoutNow();
    void checkQuorum();

    void ensureProgress();
    Progress& progressOf(const NodeId& id);
    std::vector<NodeId> ackedVoters(Index index) const;
    std::vector<NodeId> recentVoters() const;
    std::vector<NodeId> voteGranted() const;

    void broadcastAppend();
    void sendAppend(const NodeId& to);
    void sendSnapshot(const NodeId& to);
    void sendVoteRequests(bool pre_vote);
    void sendTo(Message msg);

    void rejectAppend(const Message& msg, Index hint_index, Term hint_term);
    void queueAppendResponse(const Message& msg, bool reject, Index match);
    void updateCommit(Index index);

    void failInflightProposals();
    void completeCommittedProposals();
    void ackReadsFrom(const NodeId& peer, std::uint64_t seq);
    void maybeCompleteReads();
    void dropUnstableFrom(Index index);

    std::uint64_t randU64();
    std::int64_t randomizedTimeoutMs();

    NodeId self_;
    GroupId group_ = 0;
    RaftOptions options_;
    RaftLog log_;
    Configuration conf_;
    Index config_index_ = 0;

    Term current_term_ = 0;
    NodeId voted_for_;
    Index commit_index_ = 0;
    Index applied_index_ = 0;
    Index last_persisted_ = 0;
    Index last_reported_commit_ = 0;

    RaftRole role_ = RaftRole::kFollower;
    NodeId leader_;
    bool pre_campaigning_ = false;
    bool committed_current_term_ = false;
    bool transferring_ = false;
    NodeId transfer_target_;
    std::int64_t transfer_deadline_ms_ = 0;

    std::int64_t now_ms_ = 0;
    bool clock_initialized_ = false;
    std::int64_t election_elapsed_ms_ = 0;
    std::int64_t leader_contact_elapsed_ms_ = 0;
    std::int64_t heartbeat_elapsed_ms_ = 0;
    std::int64_t check_quorum_elapsed_ms_ = 0;
    std::int64_t randomized_timeout_ms_ = 0;
    std::uint64_t rng_state_ = 1;
    std::uint64_t heartbeat_seq_ = 0;

    std::unordered_map<NodeId, Progress> progress_;
    std::unordered_map<NodeId, bool> votes_;

    bool hs_dirty_ = false;
    bool awaiting_advance_ = false;
    std::optional<Index> truncate_from_;
    std::vector<LogEntry> unstable_;
    std::optional<Snapshot> pending_snapshot_;
    std::vector<Message> messages_;
    std::vector<ProposalOutcome> proposal_outcomes_;
    std::vector<ReadyRead> ready_reads_;
    std::string snapshot_data_;
    Configuration snapshot_conf_;
    bool timeout_now_sent_ = false;

    std::vector<PendingProposal> pending_proposals_;
    std::vector<PendingRead> pending_reads_;
    ProposalId next_proposal_id_ = 1;
    ReadId next_read_id_ = 1;
};

} // namespace consensus
