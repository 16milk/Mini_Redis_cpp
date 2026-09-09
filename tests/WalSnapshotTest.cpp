#include "mini_redis/cluster/SlotMigration.hpp"
#include "mini_redis/cluster/SlotOwnership.hpp"
#include "mini_redis/consensus/RaftCore.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/persistence/Checksum.hpp"
#include "mini_redis/persistence/DurableFile.hpp"
#include "mini_redis/persistence/GroupSnapshot.hpp"
#include "mini_redis/persistence/PersistentStorage.hpp"
#include "mini_redis/persistence/Rdb.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::string makeTempDir() {
    char tmpl[] = "/tmp/mini_redis_wal_XXXXXX";
    expect(mkdtemp(tmpl) != nullptr, "create temp data dir");
    return tmpl;
}

void removeTree(const std::string& path) {
    expect(path.rfind("/tmp/mini_redis_wal_", 0) == 0, "refuse to delete unexpected path");
    const std::string command = "/bin/rm -rf '" + path + "'";
    expect(std::system(command.c_str()) == 0, "remove temp data dir");
}

struct TempDir {
    std::string path;
    explicit TempDir() : path(makeTempDir()) {}
    ~TempDir() { removeTree(path); }
};

persistence::OpenOptions testOptions(const std::string& data_dir,
                                     std::size_t segment_bytes = 64U << 10) {
    persistence::OpenOptions options;
    options.data_dir = data_dir;
    options.cluster_id = "cluster-a";
    options.node_id = "n1";
    options.group_id = 1;
    options.bootstrap.incoming.voters = {"n1", "n2", "n3"};
    options.segment_bytes = segment_bytes;
    return options;
}

consensus::LogEntry makeEntry(consensus::Index index, consensus::Term term,
                              const std::string& payload) {
    consensus::LogEntry entry;
    entry.index = index;
    entry.term = term;
    entry.type = consensus::EntryType::kCommand;
    entry.payload = payload;
    return entry;
}

consensus::HardState makeHardState(consensus::Term term, consensus::Index commit,
                                   const std::string& voted_for = "") {
    consensus::HardState hs;
    hs.current_term = term;
    hs.commit_index = commit;
    hs.voted_for = voted_for;
    return hs;
}

std::unique_ptr<persistence::PersistentStorage> mustOpen(
    const persistence::OpenOptions& options) {
    std::string error;
    auto store = persistence::PersistentStorage::open(options, error);
    expect(store != nullptr, "open storage: " + error);
    return store;
}

std::string readBytes(const std::string& path) {
    std::string bytes;
    std::string error;
    expect(persistence::readFile(path, bytes, 32U << 20, error), "read " + path + ": " + error);
    return bytes;
}

void writeBytes(const std::string& path, const std::string& bytes) {
    const int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    expect(fd != -1, "open " + path + " for rewrite");
    expect(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()),
           "rewrite " + path);
    close(fd);
}

std::uint32_t readBe32(const std::string& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3]));
}

std::uint64_t readBe64(const std::string& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(i)]);
    }
    return value;
}

void writeBe32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<char>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<char>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<char>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<char>(value & 0xff);
}

void writeBe64(std::string& bytes, std::size_t offset, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        bytes[offset + static_cast<std::size_t>(i)] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

void rewriteRecordTerm(std::string& bytes, consensus::Index target, consensus::Term term) {
    const std::uint16_t header_size =
        static_cast<std::uint16_t>((static_cast<unsigned char>(bytes[6]) << 8) |
                                   static_cast<unsigned char>(bytes[7]));
    std::size_t offset = header_size;
    while (offset + 4 <= bytes.size()) {
        const std::uint32_t body_len = readBe32(bytes, offset);
        expect(offset + 4 + body_len <= bytes.size(), "record fits in the segment");
        const std::size_t body = offset + 4;
        const consensus::Index index = readBe64(bytes, body + 1 + 8);
        if (index == target) {
            writeBe64(bytes, body + 1, term);
            const std::uint32_t crc = persistence::crc32Ieee(
                std::string_view(bytes.data() + body, body_len - 4));
            writeBe32(bytes, body + body_len - 4, crc);
            return;
        }
        offset += 4u + body_len;
    }
    expect(false, "target WAL index was not found");
}

void testAtomicReplacePreservesOriginal() {
    TempDir dir;
    const std::string path = dir.path + "/state";
    std::string error;
    expect(persistence::writeFileAtomically(path, std::string("v1"), error),
           "first atomic write: " + error);
    expect(readBytes(path) == "v1", "first contents");

    const std::string as_directory = dir.path + "/not-a-file";
    expect(mkdir(as_directory.c_str(), 0700) == 0, "create directory failure target");
    expect(!persistence::writeFileAtomically(as_directory, std::string("v2"), error),
           "write to a directory fails");
    expect(readBytes(path) == "v1", "failed write leaves the original file");
}

void testIdentityMismatchFailClosed() {
    TempDir dir;
    auto store = mustOpen(testOptions(dir.path));
    store.reset();

    auto options = testOptions(dir.path);
    options.cluster_id = "other-cluster";
    std::string error;
    expect(persistence::PersistentStorage::open(options, error) == nullptr,
           "wrong cluster id must fail");
    expect(error.find("identity") != std::string::npos, "identity mismatch is reported");

    options = testOptions(dir.path);
    options.node_id = "n9";
    expect(persistence::PersistentStorage::open(options, error) == nullptr,
           "wrong node id must fail");
}

void testBatchFsyncAndReopen() {
    TempDir dir;
    auto options = testOptions(dir.path);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, "a"), makeEntry(2, 1, "b"), makeEntry(3, 1, "c")};
    ready.hard_state = makeHardState(1, 2, "n2");
    const auto fsyncs_before = store->stats().wal_fsyncs;
    expect(store->applyReady(ready), "batch applyReady: " + store->lastError());
    expect(store->stats().wal_fsyncs == fsyncs_before + 1,
           "one WAL fsync for a batch of entries");
    expect(store->hardState().voted_for == "n2", "votedFor is persisted");
    store.reset();

    store = mustOpen(options);
    expect(store->entries().size() == 3, "reopen loads all WAL entries");
    expect(store->hardState().voted_for == "n2", "votedFor survives restart");
    expect(store->hardState().commit_index == 2, "commit index survives restart");
    const auto replay = store->committedReplay();
    expect(replay.size() == 2, "only (lastIncluded, commit] is replayed");
    expect(replay.back().payload == "b", "replay stops at commit index");
}

void testTornTailOfUncommittedRecord() {
    TempDir dir;
    auto options = testOptions(dir.path);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, "a"), makeEntry(2, 1, "b"), makeEntry(3, 1, "tail")};
    ready.hard_state = makeHardState(1, 2);
    expect(store->applyReady(ready), "append with uncommitted tail");
    const auto paths = store->walSegmentPaths();
    expect(!paths.empty(), "WAL segment exists");
    store.reset();

    std::string bytes = readBytes(paths.back());
    expect(bytes.size() > 8, "segment is large enough to tear");
    bytes.resize(bytes.size() - 5);
    writeBytes(paths.back(), bytes);

    store = mustOpen(options);
    expect(store->entries().size() == 2, "torn uncommitted tail is truncated");
    expect(store->committedReplay().size() == 2, "committed prefix is intact");
}

void testTornCommittedTailFailClosed() {
    TempDir dir;
    auto options = testOptions(dir.path);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, "a"), makeEntry(2, 1, "b")};
    ready.hard_state = makeHardState(1, 2);
    expect(store->applyReady(ready), "append committed entries");
    const auto paths = store->walSegmentPaths();
    store.reset();

    std::string bytes = readBytes(paths.back());
    bytes.resize(bytes.size() - 5);
    writeBytes(paths.back(), bytes);

    std::string error;
    expect(persistence::PersistentStorage::open(options, error) == nullptr,
           "missing committed record is fail-closed");
    expect(error.find("hole") != std::string::npos, "committed hole is reported: " + error);
}

void testMiddleChecksumFailClosed() {
    TempDir dir;
    auto options = testOptions(dir.path, 180);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, std::string(80, 'a')),
                     makeEntry(2, 1, std::string(80, 'b')),
                     makeEntry(3, 1, std::string(80, 'c'))};
    ready.hard_state = makeHardState(1, 3);
    expect(store->applyReady(ready), "append multi-segment log: " + store->lastError());
    expect(store->walSegmentCount() >= 3, "small segments force a middle file");
    const auto paths = store->walSegmentPaths();
    store.reset();

    std::string middle = readBytes(paths[1]);
    expect(middle.size() > 40, "middle segment has payload");
    middle[middle.size() / 2] ^= 0x5a;
    writeBytes(paths[1], middle);

    std::string error;
    expect(persistence::PersistentStorage::open(options, error) == nullptr,
           "middle checksum failure is fail-closed");
    expect(error.find("checksum") != std::string::npos, "checksum error is reported: " + error);
}

void testIndexHoleFailClosed() {
    TempDir dir;
    auto options = testOptions(dir.path, 180);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, std::string(80, 'a')),
                     makeEntry(2, 1, std::string(80, 'b')),
                     makeEntry(3, 1, std::string(80, 'c'))};
    ready.hard_state = makeHardState(1, 3);
    expect(store->applyReady(ready), "append for hole test");
    expect(store->walSegmentCount() >= 3, "need a middle segment to delete");
    const auto paths = store->walSegmentPaths();
    store.reset();

    expect(unlink(paths[1].c_str()) == 0, "delete middle WAL segment");
    std::string error;
    expect(persistence::PersistentStorage::open(options, error) == nullptr,
           "deleted middle segment is a hole");
    expect(error.find("hole") != std::string::npos, "index hole is reported: " + error);
}

void testTermMismatchFailClosed() {
    TempDir dir;
    auto options = testOptions(dir.path);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 2, "a"), makeEntry(2, 3, "b")};
    ready.hard_state = makeHardState(3, 2);
    expect(store->applyReady(ready), "append increasing terms");
    const auto paths = store->walSegmentPaths();
    store.reset();

    std::string bytes = readBytes(paths.back());
    rewriteRecordTerm(bytes, 2, 1);
    writeBytes(paths.back(), bytes);

    std::string error;
    auto reopened = persistence::PersistentStorage::open(options, error);
    expect(reopened == nullptr, "term regression is fail-closed: " + error);
    expect(error.find("term") != std::string::npos, "term mismatch is reported: " + error);
}

void testSnapshotRecoveryAndWalRecycle() {
    TempDir dir;
    auto options = testOptions(dir.path, 180);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, std::string(80, 'a')),
                     makeEntry(2, 1, std::string(80, 'b')),
                     makeEntry(3, 1, std::string(80, 'c'))};
    ready.hard_state = makeHardState(1, 3);
    expect(store->applyReady(ready), "seed log for snapshot");
    expect(store->walSegmentCount() >= 3, "multiple segments before snapshot");

    consensus::Snapshot snapshot;
    snapshot.last_included_index = 2;
    snapshot.last_included_term = 1;
    snapshot.conf = options.bootstrap;
    snapshot.data = "state-at-2";
    expect(store->installSnapshot(snapshot), "install snapshot: " + store->lastError());
    expect(store->entries().size() == 1, "Raft log keeps entries after lastIncluded");
    expect(store->entries().front().index == 3, "suffix starts at lastIncluded+1");
    store.reset();

    store = mustOpen(options);
    expect(store->snapshot().data == "state-at-2", "snapshot payload reloads");
    expect(store->snapshot().last_included_index == 2, "snapshot index reloads");
    const auto replay = store->committedReplay();
    expect(replay.size() == 1 && replay[0].index == 3,
           "startup replays only (lastIncluded, commit]");
}

void testWalPinPreventsRecycle() {
    TempDir dir;
    auto options = testOptions(dir.path, 180);
    auto store = mustOpen(options);
    consensus::Ready ready;
    ready.entries = {makeEntry(1, 1, std::string(80, 'a')),
                     makeEntry(2, 1, std::string(80, 'b')),
                     makeEntry(3, 1, std::string(80, 'c'))};
    ready.hard_state = makeHardState(1, 3);
    expect(store->applyReady(ready), "seed log for pin test");
    const auto before = store->walSegmentCount();
    store->pinWalAfter("migration-1", 1);
    consensus::Snapshot snapshot;
    snapshot.last_included_index = 3;
    snapshot.last_included_term = 1;
    snapshot.conf = options.bootstrap;
    snapshot.data = "state-at-3";
    expect(store->installSnapshot(snapshot), "snapshot with pin");
    expect(store->recycleThroughIndex() == 1, "pin caps recycle at the pinned index");
    expect(store->walSegmentCount() + 1 <= before + 1, "pin keeps later segments");
    expect(store->walSegmentCount() >= 1, "pinned suffix is retained");
}

void testPersistBeforeAckDriver() {
    TempDir dir;
    auto options = testOptions(dir.path);
    options.bootstrap.incoming.voters = {"n1"};
    auto store = mustOpen(options);
    consensus::RaftOptions raft_options;
    raft_options.heartbeat_interval_ms = 10;
    raft_options.election_timeout_min_ms = 40;
    raft_options.election_timeout_max_ms = 40;
    consensus::RaftCore core("n1", 1, store->toRestore(), raft_options);
    const auto drain = [&]() {
        for (int guard = 0; guard < 64 && core.hasReady(); ++guard) {
            consensus::Ready ready = core.takeReady();
            expect(store->applyReady(ready),
                   "persist Ready before sending acks: " + store->lastError());
            for (const consensus::LogEntry& entry : ready.committed) {
                core.reportApplied(entry.index);
            }
            core.advance();
        }
    };
    core.tick(0);
    drain();
    core.tick(80);
    drain();
    expect(core.role() == consensus::RaftRole::kLeader, "single voter becomes leader");
    const auto proposed = core.propose("k=v");
    expect(proposed.error == consensus::ProposeError::kOk, "leader accepts a proposal");
    drain();
    store.reset();

    store = mustOpen(options);
    bool found = false;
    for (const consensus::LogEntry& entry : store->committedReplay()) {
        if (entry.payload == "k=v") {
            found = true;
        }
    }
    expect(found, "acknowledged write is present after crash");
}

void testDataGroupSnapshotRoundTrip() {
    Database database(true);
    database.importKey("alpha", std::make_shared<StringObject>("one"), 9'000);
    auto hash = std::make_shared<HashObject>();
    hash->set_field("f", "v");
    database.importKey("beta", hash, 0);

    cluster::SlotOwnershipTable ownership;
    cluster::LocalSlotOwnership slot;
    slot.shard = 1;
    slot.epoch = 7;
    slot.state = cluster::LocalSlotState::kStable;
    std::string error;
    expect(ownership.update(42, 0, slot, error), "seed ownership: " + error);

    cluster::SlotMigrationStateMachine migration(database, ownership, 1,
                                                 cluster::MigrationKey("secret"));
    const std::string encoded =
        persistence::encodeDataGroupSnapshot(database, ownership, &migration);

    Database restored(true);
    cluster::SlotOwnershipTable restored_ownership;
    cluster::SlotMigrationStateMachine restored_migration(
        restored, restored_ownership, 1, cluster::MigrationKey("secret"));
    expect(persistence::decodeDataGroupSnapshot(encoded, restored, restored_ownership,
                                                &restored_migration, error),
           "decode data snapshot: " + error);

    StoredEntry entry;
    expect(restored.exportKey("alpha", entry), "string key restored");
    expect(entry.expire_at_ms == 9'000, "absolute TTL is restored");
    expect(restored.exportKey("beta", entry), "hash key restored");
    expect(restored_ownership.get(42).epoch == 7, "slot epoch is restored");
    expect(restored_ownership.get(42).shard == 1, "slot ownership is restored");
}

void testRdbIsNotRaftRestore() {
    TempDir dir;
    ObjectMap objects;
    objects.emplace("only-in-rdb", std::make_shared<StringObject>("secret"));
    const std::string rdb_path = dir.path + "/dump.rdb";
    expect(RdbEncoder::saveToFile(rdb_path, objects), "write dump.rdb beside WAL");

    auto store = mustOpen(testOptions(dir.path));
    expect(store->entries().empty(), "opening WAL storage ignores dump.rdb");
    expect(store->snapshot().last_included_index == 0, "RDB is not a Raft snapshot");
    store.reset();

    std::string snapshot_data;
    std::string error;
    expect(persistence::snapshotDataFromRdbFile(rdb_path, 1, snapshot_data, error),
           "RDB can be imported into snapshot bytes: " + error);
    Database imported(true);
    cluster::SlotOwnershipTable ownership;
    expect(persistence::decodeDataGroupSnapshot(snapshot_data, imported, ownership, nullptr,
                                                error),
           "imported snapshot installs: " + error);
    StoredEntry entry;
    expect(imported.exportKey("only-in-rdb", entry), "RDB import carries the object");
}

}  // namespace

int main() {
    testAtomicReplacePreservesOriginal();
    testIdentityMismatchFailClosed();
    testBatchFsyncAndReopen();
    testTornTailOfUncommittedRecord();
    testTornCommittedTailFailClosed();
    testMiddleChecksumFailClosed();
    testIndexHoleFailClosed();
    testTermMismatchFailClosed();
    testSnapshotRecoveryAndWalRecycle();
    testWalPinPreventsRecycle();
    testPersistBeforeAckDriver();
    testDataGroupSnapshotRoundTrip();
    testRdbIsNotRaftRestore();
    std::cout << "WAL, snapshot and RDB persistence tests passed" << std::endl;
    return 0;
}
