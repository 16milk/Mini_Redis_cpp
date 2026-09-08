#include "mini_redis/consensus/RaftCore.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace consensus {
namespace {

constexpr std::uint64_t kRngMul = 6364136223846793005ULL;

bool containsId(const std::vector<NodeId>& ids, const NodeId& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void eraseId(std::vector<NodeId>& ids, const NodeId& id) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
}

} // namespace

RaftCore::RaftCore(NodeId self, GroupId group, RaftRestore restore, RaftOptions options)
    : self_(std::move(self)), group_(group), options_(std::move(options)), log_(restore) {
    conf_ = normalizeConf(restore.conf);
    snapshot_conf_ = restore.snapshot_index == 0 ? conf_ : normalizeConf(restore.snapshot_conf);
    snapshot_data_ = restore.snapshot_data;
    current_term_ = restore.hard_state.current_term;
    voted_for_ = restore.hard_state.voted_for;
    commit_index_ = restore.hard_state.commit_index;
    if (commit_index_ < log_.snapshotIndex()) {
        commit_index_ = log_.snapshotIndex();
    }
    if (commit_index_ > log_.lastIndex()) {
        commit_index_ = log_.lastIndex();
    }
    applied_index_ = log_.snapshotIndex();
    last_persisted_ = log_.lastIndex();
    last_reported_commit_ = log_.snapshotIndex();

    config_index_ = log_.snapshotIndex();
    for (Index index = log_.lastIndex(); index >= log_.firstIndex() && index > 0; --index) {
        const LogEntry* entry = log_.entry(index);
        if (entry != nullptr && entry->type == EntryType::kConfig) {
            conf_ = normalizeConf(entry->config);
            config_index_ = entry->index;
            break;
        }
        if (index == 0) {
            break;
        }
    }

    rng_state_ = std::hash<std::string>{}(self_) ^ (group_ * 0x9e3779b97f4a7c15ULL) ^ 1U;
    if (rng_state_ == 0) {
        rng_state_ = 1;
    }
    randomized_timeout_ms_ = randomizedTimeoutMs();
    committed_current_term_ =
        commit_index_ > 0 && log_.term(commit_index_).value_or(0) == current_term_ &&
        current_term_ != 0;
}

Index RaftCore::matchIndex(const NodeId& peer) const {
    const auto it = progress_.find(peer);
    return it == progress_.end() ? 0 : it->second.match;
}

Index RaftCore::nextIndex(const NodeId& peer) const {
    const auto it = progress_.find(peer);
    return it == progress_.end() ? 0 : it->second.next;
}

void RaftCore::tick(std::int64_t now_ms) {
    if (clock_initialized_ && now_ms < now_ms_) {
        now_ms = now_ms_;
    }
    const std::int64_t elapsed = clock_initialized_ ? now_ms - now_ms_ : 0;
    clock_initialized_ = true;
    now_ms_ = now_ms;
    if (awaiting_advance_) {
        return;
    }

    if (role_ == RaftRole::kLeader) {
        heartbeat_elapsed_ms_ += elapsed;
        check_quorum_elapsed_ms_ += elapsed;
        if (transferring_ && now_ms_ >= transfer_deadline_ms_) {
            abortTransfer();
        }
        if (options_.check_quorum &&
            check_quorum_elapsed_ms_ >= options_.election_timeout_min_ms) {
            checkQuorum();
            check_quorum_elapsed_ms_ = 0;
            if (role_ != RaftRole::kLeader) {
                return;
            }
        }
        if (heartbeat_elapsed_ms_ >= options_.heartbeat_interval_ms) {
            heartbeat_elapsed_ms_ = 0;
            ++heartbeat_seq_;
            broadcastAppend();
        }
        return;
    }

    election_elapsed_ms_ += elapsed;
    leader_contact_elapsed_ms_ += elapsed;
    if (election_elapsed_ms_ >= randomized_timeout_ms_) {
        election_elapsed_ms_ = 0;
        if (isVoter(conf_, self_)) {
            campaign(options_.pre_vote);
        } else {
            resetElectionTimer();
        }
    }
}

void RaftCore::step(Message msg) {
    if (msg.group != group_ || (!msg.to.empty() && msg.to != self_)) {
        return;
    }
    if (awaiting_advance_) {
        return;
    }

    const bool pre_vote = msg.type == MessageType::kPreVote ||
                          msg.type == MessageType::kPreVoteResponse;
    if (pre_vote) {
        if (msg.type == MessageType::kPreVote) {
            handleVoteRequest(msg, true);
        } else {
            handleVoteResponse(msg, true);
        }
        return;
    }

    if (msg.term > current_term_) {
        NodeId new_leader;
        if (msg.type == MessageType::kAppendEntries ||
            msg.type == MessageType::kInstallSnapshot) {
            new_leader = msg.from;
        }
        becomeFollower(msg.term, new_leader);
    } else if (msg.term < current_term_) {
        if (msg.type == MessageType::kAppendEntries) {
            rejectAppend(msg, log_.lastIndex(), log_.lastTerm());
        } else if (msg.type == MessageType::kRequestVote) {
            Message resp;
            resp.type = MessageType::kRequestVoteResponse;
            resp.to = msg.from;
            resp.term = current_term_;
            resp.reject = true;
            sendTo(std::move(resp));
        }
        return;
    }

    switch (msg.type) {
        case MessageType::kAppendEntries:
            handleAppendEntries(msg);
            break;
        case MessageType::kAppendEntriesResponse:
            handleAppendEntriesResponse(msg);
            break;
        case MessageType::kRequestVote:
            handleVoteRequest(msg, false);
            break;
        case MessageType::kRequestVoteResponse:
            handleVoteResponse(msg, false);
            break;
        case MessageType::kTimeoutNow:
            handleTimeoutNow(msg);
            break;
        case MessageType::kInstallSnapshot:
            handleInstallSnapshot(msg);
            break;
        case MessageType::kInstallSnapshotResponse:
            handleInstallSnapshotResponse(msg);
            break;
        case MessageType::kPreVote:
        case MessageType::kPreVoteResponse:
            break;
    }
}

ProposeResult RaftCore::propose(std::string payload) {
    ProposeResult result;
    result.leader_hint = leader_;
    if (role_ != RaftRole::kLeader) {
        result.error = ProposeError::kNotLeader;
        return result;
    }
    if (transferring_) {
        result.error = ProposeError::kTransferring;
        return result;
    }
    if (!isVoter(conf_, self_)) {
        result.error = ProposeError::kNotVoter;
        return result;
    }
    LogEntry entry;
    entry.type = EntryType::kCommand;
    entry.payload = std::move(payload);
    appendEntry(std::move(entry));
    result.id = next_proposal_id_++;
    result.index = log_.lastIndex();
    pending_proposals_.push_back(PendingProposal{result.id, result.index, current_term_});
    return result;
}

ProposeResult RaftCore::addLearner(const NodeId& node_id) {
    if (role_ != RaftRole::kLeader) {
        ProposeResult result;
        result.error = ProposeError::kNotLeader;
        result.leader_hint = leader_;
        return result;
    }
    if (conf_.joint() || config_index_ > commit_index_) {
        ProposeResult result;
        result.error = ProposeError::kBusyConfig;
        return result;
    }
    if (node_id.empty() || isMember(conf_, node_id)) {
        ProposeResult result;
        result.error = ProposeError::kUnknownPeer;
        return result;
    }
    Configuration next;
    next.incoming = conf_.incoming;
    next.incoming.learners.push_back(node_id);
    return proposeConfig(normalizeConf(std::move(next)), "add-learner:" + node_id);
}

ProposeResult RaftCore::promoteLearner(const NodeId& node_id) {
    if (role_ != RaftRole::kLeader) {
        ProposeResult result;
        result.error = ProposeError::kNotLeader;
        result.leader_hint = leader_;
        return result;
    }
    if (conf_.joint() || config_index_ > commit_index_) {
        ProposeResult result;
        result.error = ProposeError::kBusyConfig;
        return result;
    }
    if (!isLearner(conf_, node_id)) {
        ProposeResult result;
        result.error = ProposeError::kUnknownPeer;
        return result;
    }
    const Index match = matchIndex(node_id);
    if (log_.lastIndex() > match &&
        log_.lastIndex() - match > options_.learner_promote_max_lag) {
        ProposeResult result;
        result.error = ProposeError::kLearnerLagging;
        return result;
    }
    Membership old_membership = conf_.incoming;
    Membership new_membership = conf_.incoming;
    new_membership.voters.push_back(node_id);
    eraseId(new_membership.learners, node_id);
    return proposeConfig(jointConf(old_membership, new_membership), "promote:" + node_id);
}

ProposeResult RaftCore::removeNode(const NodeId& node_id) {
    if (role_ != RaftRole::kLeader) {
        ProposeResult result;
        result.error = ProposeError::kNotLeader;
        result.leader_hint = leader_;
        return result;
    }
    if (conf_.joint() || config_index_ > commit_index_) {
        ProposeResult result;
        result.error = ProposeError::kBusyConfig;
        return result;
    }
    if (!isMember(conf_, node_id)) {
        ProposeResult result;
        result.error = ProposeError::kUnknownPeer;
        return result;
    }
    if (node_id == self_ && isVoter(conf_, self_)) {
        ProposeResult result;
        result.error = ProposeError::kRemovingLeader;
        return result;
    }
    Membership old_membership = conf_.incoming;
    Membership new_membership = conf_.incoming;
    eraseId(new_membership.voters, node_id);
    eraseId(new_membership.learners, node_id);
    if (containsId(old_membership.voters, node_id)) {
        return proposeConfig(jointConf(old_membership, new_membership), "remove:" + node_id);
    }
    Configuration next;
    next.incoming = new_membership;
    return proposeConfig(normalizeConf(std::move(next)), "remove-learner:" + node_id);
}

ProposeResult RaftCore::demoteVoter(const NodeId& node_id) {
    if (role_ != RaftRole::kLeader) {
        ProposeResult result;
        result.error = ProposeError::kNotLeader;
        result.leader_hint = leader_;
        return result;
    }
    if (conf_.joint() || config_index_ > commit_index_) {
        ProposeResult result;
        result.error = ProposeError::kBusyConfig;
        return result;
    }
    if (!containsId(conf_.incoming.voters, node_id)) {
        ProposeResult result;
        result.error = ProposeError::kUnknownPeer;
        return result;
    }
    if (node_id == self_) {
        ProposeResult result;
        result.error = ProposeError::kRemovingLeader;
        return result;
    }
    Membership old_membership = conf_.incoming;
    Membership new_membership = conf_.incoming;
    eraseId(new_membership.voters, node_id);
    if (!containsId(new_membership.learners, node_id)) {
        new_membership.learners.push_back(node_id);
    }
    return proposeConfig(jointConf(old_membership, new_membership), "demote:" + node_id);
}

bool RaftCore::transferLeadership(const NodeId& target) {
    if (role_ != RaftRole::kLeader || target == self_ || !isVoter(conf_, target)) {
        return false;
    }
    transferring_ = true;
    transfer_target_ = target;
    transfer_deadline_ms_ = now_ms_ + options_.transfer_timeout_ms;
    timeout_now_sent_ = false;
    sendAppend(target);
    maybeSendTimeoutNow();
    return true;
}

ReadRequest RaftCore::readIndex() {
    ReadRequest request;
    request.leader_hint = leader_;
    if (role_ != RaftRole::kLeader) {
        request.error = ReadError::kNotLeader;
        return request;
    }
    if (transferring_) {
        request.error = ReadError::kTransferring;
        return request;
    }
    if (!isVoter(conf_, self_)) {
        request.error = ReadError::kNotVoter;
        return request;
    }
    request.id = next_read_id_++;
    request.read_index = commit_index_;
    PendingRead pending;
    pending.id = request.id;
    pending.read_index = commit_index_;
    if (committed_current_term_ && allVoters(conf_).size() == 1) {
        pending.quorum_acked = true;
    } else if (committed_current_term_) {
        ++heartbeat_seq_;
        pending.need_seq = heartbeat_seq_;
        broadcastAppend();
    }
    pending_reads_.push_back(pending);
    maybeCompleteReads();
    return request;
}

void RaftCore::reportApplied(Index index) {
    if (index > applied_index_ && index <= commit_index_) {
        applied_index_ = index;
        maybeCompleteReads();
    }
}

bool RaftCore::compact(Index index, std::string snapshot_data) {
    if (index == 0 || index > applied_index_ || index > commit_index_) {
        return false;
    }
    if (index < log_.snapshotIndex()) {
        return false;
    }
    if (index == log_.snapshotIndex()) {
        snapshot_data_ = std::move(snapshot_data);
        return true;
    }
    const std::optional<Term> compact_term = log_.term(index);
    if (!compact_term.has_value()) {
        return false;
    }
    Configuration conf_at = snapshot_conf_;
    for (Index scan = log_.firstIndex(); scan <= index; ++scan) {
        const LogEntry* entry = log_.entry(scan);
        if (entry != nullptr && entry->type == EntryType::kConfig) {
            conf_at = entry->config;
        }
    }
    log_.compact(index, *compact_term);
    snapshot_conf_ = normalizeConf(std::move(conf_at));
    snapshot_data_ = std::move(snapshot_data);
    Snapshot snapshot;
    snapshot.last_included_index = index;
    snapshot.last_included_term = *compact_term;
    snapshot.conf = snapshot_conf_;
    snapshot.data = snapshot_data_;
    pending_snapshot_ = snapshot;
    unstable_.erase(std::remove_if(unstable_.begin(), unstable_.end(),
                                   [index](const LogEntry& entry) { return entry.index <= index; }),
                    unstable_.end());
    return true;
}

bool RaftCore::hasReady() const {
    if (awaiting_advance_) {
        return false;
    }
    return hs_dirty_ || truncate_from_.has_value() || !unstable_.empty() ||
           pending_snapshot_.has_value() || !messages_.empty() ||
           commit_index_ > last_reported_commit_ || !proposal_outcomes_.empty() ||
           !ready_reads_.empty();
}

Ready RaftCore::takeReady() {
    Ready ready;
    if (hs_dirty_) {
        HardState hs;
        hs.current_term = current_term_;
        hs.voted_for = voted_for_;
        hs.commit_index = commit_index_;
        ready.hard_state = hs;
        hs_dirty_ = false;
    }
    ready.truncate_from = truncate_from_;
    truncate_from_.reset();
    ready.entries = std::move(unstable_);
    unstable_.clear();
    ready.snapshot = std::move(pending_snapshot_);
    pending_snapshot_.reset();
    ready.messages = std::move(messages_);
    messages_.clear();
    if (commit_index_ > last_reported_commit_) {
        ready.committed = log_.slice(last_reported_commit_ + 1, commit_index_ + 1);
        last_reported_commit_ = commit_index_;
    }
    ready.proposals = std::move(proposal_outcomes_);
    proposal_outcomes_.clear();
    ready.reads = std::move(ready_reads_);
    ready_reads_.clear();
    awaiting_advance_ = true;
    return ready;
}

void RaftCore::advance() {
    awaiting_advance_ = false;
    const Index previous_persisted = last_persisted_;
    const Index previous_commit = commit_index_;
    last_persisted_ = log_.lastIndex();
    if (log_.snapshotIndex() > last_persisted_) {
        last_persisted_ = log_.snapshotIndex();
    }
    if (role_ == RaftRole::kLeader) {
        Progress& self_progress = progressOf(self_);
        self_progress.match = last_persisted_;
        self_progress.next = last_persisted_ + 1;
        self_progress.acked_seq = heartbeat_seq_;
        maybeCommit();
        if (last_persisted_ > previous_persisted || commit_index_ > previous_commit) {
            broadcastAppend();
        }
        maybeSendTimeoutNow();
        maybeCompleteReads();
    }
}

void RaftCore::becomeFollower(Term term, NodeId leader) {
    if (term > current_term_) {
        current_term_ = term;
        voted_for_.clear();
        hs_dirty_ = true;
    }
    const bool was_leader = role_ == RaftRole::kLeader;
    role_ = RaftRole::kFollower;
    leader_ = std::move(leader);
    pre_campaigning_ = false;
    votes_.clear();
    progress_.clear();
    committed_current_term_ = false;
    abortTransfer();
    if (was_leader) {
        failInflightProposals();
        pending_reads_.clear();
    }
    resetElectionTimer();
}

void RaftCore::becomeCandidate() {
    role_ = RaftRole::kCandidate;
    leader_.clear();
    pre_campaigning_ = false;
    ++current_term_;
    voted_for_ = self_;
    hs_dirty_ = true;
    votes_.clear();
    votes_[self_] = true;
    committed_current_term_ = false;
    abortTransfer();
    resetElectionTimer();
}

void RaftCore::becomeLeader() {
    role_ = RaftRole::kLeader;
    leader_ = self_;
    pre_campaigning_ = false;
    votes_.clear();
    transferring_ = false;
    timeout_now_sent_ = false;
    committed_current_term_ = false;
    heartbeat_elapsed_ms_ = 0;
    check_quorum_elapsed_ms_ = 0;
    ensureProgress();
    for (auto& item : progress_) {
        item.second.next = log_.lastIndex() + 1;
        item.second.match = item.first == self_ ? last_persisted_ : 0;
        item.second.recent = item.first == self_;
        item.second.snapshot_inflight = false;
        item.second.acked_seq = 0;
    }
    LogEntry barrier;
    barrier.type = EntryType::kNoOp;
    appendEntry(std::move(barrier));
}

void RaftCore::campaign(bool pre_vote) {
    if (!isVoter(conf_, self_)) {
        return;
    }
    // 选举超时意味着不再相信旧 Leader；否则 PreVote 会因为
    // “还记着已死的 leader_ 且刚重置了计时器”而互相拒绝。
    leader_.clear();
    if (pre_vote) {
        role_ = RaftRole::kFollower;
        pre_campaigning_ = true;
        votes_.clear();
        votes_[self_] = true;
        resetElectionTimer();
        if (quorumAcked(conf_, voteGranted())) {
            campaign(false);
            return;
        }
        sendVoteRequests(true);
        return;
    }
    becomeCandidate();
    if (quorumAcked(conf_, voteGranted())) {
        becomeLeader();
        return;
    }
    sendVoteRequests(false);
}

void RaftCore::resetElectionTimer() {
    election_elapsed_ms_ = 0;
    randomized_timeout_ms_ = randomizedTimeoutMs();
}

void RaftCore::abortTransfer() {
    transferring_ = false;
    transfer_target_.clear();
    timeout_now_sent_ = false;
}

void RaftCore::handleAppendEntries(const Message& msg) {
    if (role_ != RaftRole::kFollower) {
        becomeFollower(current_term_, msg.from);
    } else {
        leader_ = msg.from;
    }
    resetElectionTimer();
    leader_contact_elapsed_ms_ = 0;

    Index truncated_from = 0;
    if (!log_.maybeAppend(msg.prev_log_index, msg.prev_log_term, msg.entries, truncated_from)) {
        const Index hint = std::min(msg.prev_log_index, log_.lastIndex());
        rejectAppend(msg, hint, log_.term(hint).value_or(0));
        return;
    }
    if (truncated_from != 0) {
        if (!truncate_from_.has_value() || truncated_from < *truncate_from_) {
            truncate_from_ = truncated_from;
        }
        dropUnstableFrom(truncated_from);
        if (last_persisted_ >= truncated_from) {
            last_persisted_ = truncated_from - 1;
        }
    }
    for (const LogEntry& entry : msg.entries) {
        const bool already =
            std::any_of(unstable_.begin(), unstable_.end(),
                        [&entry](const LogEntry& local) { return local.index == entry.index; });
        if (!already && entry.index > last_persisted_) {
            unstable_.push_back(entry);
        }
        if (entry.type == EntryType::kConfig) {
            applyConfigFromEntry(entry);
        }
    }
    const Index last_new = msg.prev_log_index + static_cast<Index>(msg.entries.size());
    if (msg.leader_commit > commit_index_) {
        updateCommit(std::min(msg.leader_commit, last_new));
    }
    queueAppendResponse(msg, false, last_new);
}

void RaftCore::handleAppendEntriesResponse(const Message& msg) {
    if (role_ != RaftRole::kLeader) {
        return;
    }
    Progress& progress = progressOf(msg.from);
    progress.recent = true;
    if (msg.reject) {
        progress.snapshot_inflight = false;
        Index next = progress.next;
        if (next > 1) {
            --next;
        }
        if (msg.reject_hint_index != 0) {
            next = std::min(next, msg.reject_hint_index + 1);
        }
        progress.next = std::max(log_.snapshotIndex() + 1, next);
        sendAppend(msg.from);
        return;
    }
    if (msg.match_index > progress.match) {
        progress.match = msg.match_index;
    }
    progress.next = std::max(progress.next, msg.match_index + 1);
    progress.snapshot_inflight = false;
    ackReadsFrom(msg.from, msg.seq);
    maybeCommit();
    maybeSendTimeoutNow();
    if (progress.next <= last_persisted_) {
        sendAppend(msg.from);
    }
}

void RaftCore::handleVoteRequest(const Message& msg, bool pre_vote) {
    const Term request_term = msg.term;
    Message resp;
    resp.type = pre_vote ? MessageType::kPreVoteResponse : MessageType::kRequestVoteResponse;
    resp.to = msg.from;
    resp.reject = true;
    resp.term = pre_vote ? request_term : current_term_;

    if (request_term < current_term_) {
        resp.term = current_term_;
        sendTo(std::move(resp));
        return;
    }

    const bool heard_from_leader =
        role_ == RaftRole::kLeader ||
        (!leader_.empty() && leader_contact_elapsed_ms_ < options_.election_timeout_min_ms);
    if (pre_vote && heard_from_leader) {
        resp.term = current_term_;
        sendTo(std::move(resp));
        return;
    }

    if (!pre_vote) {
        resp.term = current_term_;
    }
    const bool log_ok = logUpToDate(msg.last_log_index, msg.last_log_term);
    const bool vote_ok = pre_vote ? (request_term > current_term_ || canGrantVote(msg.from))
                                  : canGrantVote(msg.from);
    if (log_ok && vote_ok) {
        resp.reject = false;
        if (!pre_vote) {
            voted_for_ = msg.from;
            hs_dirty_ = true;
            resetElectionTimer();
        }
    }
    sendTo(std::move(resp));
}

void RaftCore::handleVoteResponse(const Message& msg, bool pre_vote) {
    if (pre_vote) {
        if (!pre_campaigning_) {
            return;
        }
        if (msg.reject) {
            if (msg.term > current_term_) {
                becomeFollower(msg.term, NodeId{});
            }
            return;
        }
        if (msg.term != current_term_ + 1) {
            return;
        }
        votes_[msg.from] = true;
        if (quorumAcked(conf_, voteGranted())) {
            campaign(false);
        }
        return;
    }
    if (role_ != RaftRole::kCandidate) {
        return;
    }
    if (msg.reject) {
        return;
    }
    votes_[msg.from] = true;
    if (quorumAcked(conf_, voteGranted())) {
        becomeLeader();
    }
}

void RaftCore::handleTimeoutNow(const Message& msg) {
    (void)msg;
    if (!isVoter(conf_, self_)) {
        return;
    }
    // Leader Transfer 是明确交权：跳过 PreVote，立即进入真正选举，
    // 这样旧 Leader 同 Term 的心跳不会把这次竞选打回 follower。
    campaign(false);
}

void RaftCore::handleInstallSnapshot(const Message& msg) {
    leader_ = msg.from;
    resetElectionTimer();
    leader_contact_elapsed_ms_ = 0;
    if (msg.snapshot.last_included_index <= commit_index_) {
        Message resp;
        resp.type = MessageType::kInstallSnapshotResponse;
        resp.to = msg.from;
        resp.term = current_term_;
        resp.match_index = commit_index_;
        resp.seq = msg.seq;
        sendTo(std::move(resp));
        return;
    }
    log_.installSnapshot(msg.snapshot);
    snapshot_conf_ = normalizeConf(msg.snapshot.conf);
    snapshot_data_ = msg.snapshot.data;
    conf_ = snapshot_conf_;
    config_index_ = msg.snapshot.last_included_index;
    pending_snapshot_ = msg.snapshot;
    unstable_.clear();
    truncate_from_.reset();
    failInflightProposals();
    last_reported_commit_ = std::max(last_reported_commit_, msg.snapshot.last_included_index);
    updateCommit(msg.snapshot.last_included_index);
    Message resp;
    resp.type = MessageType::kInstallSnapshotResponse;
    resp.to = msg.from;
    resp.term = current_term_;
    resp.match_index = msg.snapshot.last_included_index;
    resp.seq = msg.seq;
    sendTo(std::move(resp));
}

void RaftCore::handleInstallSnapshotResponse(const Message& msg) {
    if (role_ != RaftRole::kLeader || msg.reject) {
        return;
    }
    Progress& progress = progressOf(msg.from);
    progress.recent = true;
    progress.snapshot_inflight = false;
    if (msg.match_index > progress.match) {
        progress.match = msg.match_index;
    }
    progress.next = std::max(progress.next, msg.match_index + 1);
    ackReadsFrom(msg.from, msg.seq);
    maybeCommit();
    sendAppend(msg.from);
}

bool RaftCore::logUpToDate(Index last_index, Term last_term) const {
    const Term local_term = log_.lastTerm();
    if (last_term != local_term) {
        return last_term > local_term;
    }
    return last_index >= log_.lastIndex();
}

bool RaftCore::canGrantVote(const NodeId& candidate) const {
    return voted_for_.empty() || voted_for_ == candidate;
}

void RaftCore::appendEntry(LogEntry entry) {
    entry.term = current_term_;
    entry.index = log_.lastIndex() + 1;
    log_.append({entry});
    unstable_.push_back(entry);
    if (entry.type == EntryType::kConfig) {
        applyConfigFromEntry(entry);
    }
}

ProposeResult RaftCore::proposeConfig(Configuration conf, std::string payload) {
    ProposeResult result;
    result.leader_hint = leader_;
    if (role_ != RaftRole::kLeader) {
        result.error = ProposeError::kNotLeader;
        return result;
    }
    if (transferring_) {
        result.error = ProposeError::kTransferring;
        return result;
    }
    LogEntry entry;
    entry.type = EntryType::kConfig;
    entry.payload = std::move(payload);
    entry.config = normalizeConf(std::move(conf));
    appendEntry(std::move(entry));
    result.id = next_proposal_id_++;
    result.index = log_.lastIndex();
    pending_proposals_.push_back(PendingProposal{result.id, result.index, current_term_});
    return result;
}

void RaftCore::applyConfigFromEntry(const LogEntry& entry) {
    conf_ = normalizeConf(entry.config);
    config_index_ = entry.index;
    if (role_ == RaftRole::kLeader) {
        ensureProgress();
        if (!isVoter(conf_, self_)) {
            becomeFollower(current_term_, NodeId{});
        }
    }
}

void RaftCore::maybeCommit() {
    if (role_ != RaftRole::kLeader) {
        return;
    }
    for (Index index = last_persisted_; index > commit_index_; --index) {
        const std::optional<Term> entry_term = log_.term(index);
        if (!entry_term.has_value() || *entry_term != current_term_) {
            continue;
        }
        if (quorumAcked(conf_, ackedVoters(index))) {
            updateCommit(index);
            return;
        }
    }
}

void RaftCore::maybeLeaveJoint() {
    if (role_ != RaftRole::kLeader || !conf_.joint() || transferring_) {
        return;
    }
    if (config_index_ > commit_index_) {
        return;
    }
    proposeConfig(leaveJoint(conf_), "leave-joint");
}

void RaftCore::maybeSendTimeoutNow() {
    if (!transferring_ || role_ != RaftRole::kLeader || timeout_now_sent_) {
        return;
    }
    const Progress& progress = progressOf(transfer_target_);
    if (progress.match >= last_persisted_ && last_persisted_ == log_.lastIndex()) {
        Message msg;
        msg.type = MessageType::kTimeoutNow;
        msg.to = transfer_target_;
        msg.term = current_term_;
        sendTo(std::move(msg));
        timeout_now_sent_ = true;
    }
}

void RaftCore::checkQuorum() {
    if (role_ != RaftRole::kLeader) {
        return;
    }
    if (!quorumAcked(conf_, recentVoters())) {
        becomeFollower(current_term_, NodeId{});
        return;
    }
    for (auto& item : progress_) {
        item.second.recent = item.first == self_;
    }
}

void RaftCore::ensureProgress() {
    std::vector<NodeId> members = allPeers(conf_, NodeId{});
    if (!containsId(members, self_)) {
        members.push_back(self_);
    }
    for (const NodeId& id : members) {
        if (progress_.find(id) == progress_.end()) {
            Progress progress;
            progress.next = log_.lastIndex() + 1;
            progress.match = id == self_ ? last_persisted_ : 0;
            progress.recent = id == self_;
            progress_[id] = progress;
        }
    }
    for (auto it = progress_.begin(); it != progress_.end();) {
        if (it->first != self_ && !isMember(conf_, it->first)) {
            it = progress_.erase(it);
        } else {
            ++it;
        }
    }
}

RaftCore::Progress& RaftCore::progressOf(const NodeId& id) {
    auto it = progress_.find(id);
    if (it == progress_.end()) {
        Progress progress;
        progress.next = log_.lastIndex() + 1;
        it = progress_.emplace(id, progress).first;
    }
    return it->second;
}

std::vector<NodeId> RaftCore::ackedVoters(Index index) const {
    std::vector<NodeId> acked;
    for (const NodeId& voter : allVoters(conf_)) {
        const auto it = progress_.find(voter);
        const Index match = it == progress_.end() ? 0 : it->second.match;
        if (match >= index) {
            acked.push_back(voter);
        }
    }
    return acked;
}

std::vector<NodeId> RaftCore::recentVoters() const {
    std::vector<NodeId> acked;
    for (const NodeId& voter : allVoters(conf_)) {
        if (voter == self_) {
            acked.push_back(voter);
            continue;
        }
        const auto it = progress_.find(voter);
        if (it != progress_.end() && it->second.recent) {
            acked.push_back(voter);
        }
    }
    return acked;
}

std::vector<NodeId> RaftCore::voteGranted() const {
    std::vector<NodeId> granted;
    for (const auto& vote : votes_) {
        if (vote.second) {
            granted.push_back(vote.first);
        }
    }
    return granted;
}

void RaftCore::broadcastAppend() {
    if (role_ != RaftRole::kLeader) {
        return;
    }
    for (const NodeId& peer : allPeers(conf_, self_)) {
        sendAppend(peer);
    }
}

void RaftCore::sendAppend(const NodeId& to) {
    if (role_ != RaftRole::kLeader || to == self_) {
        return;
    }
    Progress& progress = progressOf(to);
    if (progress.next <= log_.snapshotIndex()) {
        sendSnapshot(to);
        return;
    }
    Index next = progress.next;
    if (next > last_persisted_ + 1) {
        next = last_persisted_ + 1;
        progress.next = next;
    }
    Message msg;
    msg.type = MessageType::kAppendEntries;
    msg.to = to;
    msg.term = current_term_;
    msg.prev_log_index = next == 0 ? 0 : next - 1;
    msg.prev_log_term = log_.term(msg.prev_log_index).value_or(0);
    msg.leader_commit = commit_index_;
    msg.seq = heartbeat_seq_;
    const Index limit = std::min(last_persisted_ + 1, next + options_.max_entries_per_append);
    msg.entries = log_.slice(next, limit);
    sendTo(std::move(msg));
}

void RaftCore::sendSnapshot(const NodeId& to) {
    Progress& progress = progressOf(to);
    if (progress.snapshot_inflight) {
        return;
    }
    Message msg;
    msg.type = MessageType::kInstallSnapshot;
    msg.to = to;
    msg.term = current_term_;
    msg.seq = heartbeat_seq_;
    msg.snapshot.last_included_index = log_.snapshotIndex();
    msg.snapshot.last_included_term = log_.snapshotTerm();
    msg.snapshot.conf = snapshot_conf_;
    msg.snapshot.data = snapshot_data_;
    progress.snapshot_inflight = true;
    sendTo(std::move(msg));
}

void RaftCore::sendVoteRequests(bool pre_vote) {
    for (const NodeId& voter : allVoters(conf_)) {
        if (voter == self_) {
            continue;
        }
        Message msg;
        msg.type = pre_vote ? MessageType::kPreVote : MessageType::kRequestVote;
        msg.to = voter;
        msg.term = pre_vote ? current_term_ + 1 : current_term_;
        msg.last_log_index = log_.lastIndex();
        msg.last_log_term = log_.lastTerm();
        sendTo(std::move(msg));
    }
}

void RaftCore::sendTo(Message msg) {
    msg.from = self_;
    msg.group = group_;
    if (msg.term == 0) {
        msg.term = current_term_;
    }
    messages_.push_back(std::move(msg));
}

void RaftCore::rejectAppend(const Message& msg, Index hint_index, Term hint_term) {
    Message resp;
    resp.type = MessageType::kAppendEntriesResponse;
    resp.to = msg.from;
    resp.term = current_term_;
    resp.reject = true;
    resp.reject_hint_index = hint_index;
    resp.reject_hint_term = hint_term;
    resp.seq = msg.seq;
    sendTo(std::move(resp));
}

void RaftCore::queueAppendResponse(const Message& msg, bool reject, Index match) {
    Message resp;
    resp.type = MessageType::kAppendEntriesResponse;
    resp.to = msg.from;
    resp.term = current_term_;
    resp.reject = reject;
    resp.match_index = match;
    resp.seq = msg.seq;
    sendTo(std::move(resp));
}

void RaftCore::updateCommit(Index index) {
    if (index <= commit_index_) {
        return;
    }
    commit_index_ = index;
    hs_dirty_ = true;
    if (log_.term(index).value_or(0) == current_term_ && current_term_ != 0) {
        committed_current_term_ = true;
    }
    completeCommittedProposals();
    maybeLeaveJoint();
    maybeCompleteReads();
}

void RaftCore::failInflightProposals() {
    for (const PendingProposal& pending : pending_proposals_) {
        ProposalOutcome outcome;
        outcome.id = pending.id;
        outcome.index = pending.index;
        outcome.committed = false;
        proposal_outcomes_.push_back(outcome);
    }
    pending_proposals_.clear();
}

void RaftCore::completeCommittedProposals() {
    std::vector<PendingProposal> remaining;
    for (const PendingProposal& pending : pending_proposals_) {
        if (pending.index > commit_index_) {
            remaining.push_back(pending);
            continue;
        }
        ProposalOutcome outcome;
        outcome.id = pending.id;
        outcome.index = pending.index;
        const LogEntry* entry = log_.entry(pending.index);
        outcome.committed = entry != nullptr && entry->term == pending.term;
        proposal_outcomes_.push_back(outcome);
    }
    pending_proposals_ = std::move(remaining);
}

void RaftCore::ackReadsFrom(const NodeId& peer, std::uint64_t seq) {
    Progress& progress = progressOf(peer);
    if (seq > progress.acked_seq) {
        progress.acked_seq = seq;
    }
    maybeCompleteReads();
}

void RaftCore::maybeCompleteReads() {
    if (role_ != RaftRole::kLeader) {
        return;
    }
    std::vector<PendingRead> remaining;
    bool need_broadcast = false;
    for (PendingRead& read : pending_reads_) {
        if (!committed_current_term_) {
            remaining.push_back(read);
            continue;
        }
        if (!read.quorum_acked) {
            if (allVoters(conf_).size() == 1) {
                read.quorum_acked = true;
            } else {
                if (read.need_seq == 0) {
                    ++heartbeat_seq_;
                    read.need_seq = heartbeat_seq_;
                    need_broadcast = true;
                }
                std::vector<NodeId> acked;
                for (const NodeId& voter : allVoters(conf_)) {
                    if (voter == self_ ||
                        (progress_.count(voter) != 0 &&
                         progress_.at(voter).acked_seq >= read.need_seq)) {
                        acked.push_back(voter);
                    }
                }
                read.quorum_acked = quorumAcked(conf_, acked);
            }
        }
        if (read.quorum_acked && applied_index_ >= read.read_index) {
            ReadyRead ready;
            ready.id = read.id;
            ready.read_index = read.read_index;
            ready_reads_.push_back(ready);
        } else {
            remaining.push_back(read);
        }
    }
    pending_reads_ = std::move(remaining);
    if (need_broadcast) {
        broadcastAppend();
    }
}

void RaftCore::dropUnstableFrom(Index index) {
    unstable_.erase(std::remove_if(unstable_.begin(), unstable_.end(),
                                   [index](const LogEntry& entry) { return entry.index >= index; }),
                    unstable_.end());
    std::vector<PendingProposal> remaining;
    for (const PendingProposal& pending : pending_proposals_) {
        if (pending.index >= index) {
            ProposalOutcome outcome;
            outcome.id = pending.id;
            outcome.index = pending.index;
            outcome.committed = false;
            proposal_outcomes_.push_back(outcome);
        } else {
            remaining.push_back(pending);
        }
    }
    pending_proposals_ = std::move(remaining);
}

std::uint64_t RaftCore::randU64() {
    rng_state_ = rng_state_ * kRngMul + 1;
    return rng_state_;
}

std::int64_t RaftCore::randomizedTimeoutMs() {
    const std::int64_t min_ms = options_.election_timeout_min_ms;
    const std::int64_t max_ms = options_.election_timeout_max_ms;
    if (max_ms <= min_ms) {
        return min_ms;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(max_ms - min_ms);
    return min_ms + static_cast<std::int64_t>(randU64() % (span + 1));
}

} // namespace consensus
