#include "mini_redis/persistence/PersistentStorage.hpp"

#include "mini_redis/persistence/Checksum.hpp"
#include "mini_redis/persistence/Codec.hpp"
#include "mini_redis/persistence/DurableFile.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace persistence {
namespace {

constexpr char kIdentityMagic[] = {'M', 'R', 'I', 'D'};
constexpr char kHardStateMagic[] = {'M', 'R', 'H', 'S'};
constexpr char kCurrentMagic[] = {'M', 'R', 'S', 'C'};
constexpr char kManifestMagic[] = {'M', 'R', 'S', 'M'};
constexpr char kWalMagic[] = {'M', 'R', 'W', 'L'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint8_t kRecordEntry = 1;
constexpr std::size_t kMaxIdentityBytes = 64U << 10;
constexpr std::size_t kMaxHardStateBytes = 1U << 20;
constexpr std::size_t kMaxManifestBytes = 1U << 20;
constexpr std::size_t kMaxPayloadBytes = 1024U << 20;
constexpr std::size_t kMaxSegmentBytes = 128U << 20;
constexpr std::size_t kMaxRecordBytes = 64U << 20;
constexpr std::uint32_t kMaxMembers = 1024;
constexpr std::size_t kMaxIdBytes = 256;

std::string padU64(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llu",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string joinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

void appendCrc(ByteWriter& writer) {
    const std::uint32_t crc = crc32Ieee(writer.data());
    writer.u32(crc);
}

bool checkCrc(std::string_view bytes, std::string& error, const char* label) {
    if (bytes.size() < 4) {
        error = std::string(label) + " is too short for a checksum";
        return false;
    }
    const std::string_view body(bytes.data(), bytes.size() - 4);
    ByteReader reader(std::string_view(bytes.data() + bytes.size() - 4, 4));
    std::uint32_t stored = 0;
    if (!reader.u32(stored) || stored != crc32Ieee(body)) {
        error = std::string(label) + " checksum mismatch";
        return false;
    }
    return true;
}

void encodeMembership(ByteWriter& writer, const consensus::Membership& membership) {
    writer.u32(static_cast<std::uint32_t>(membership.voters.size()));
    for (const consensus::NodeId& id : membership.voters) {
        writer.str(id);
    }
    writer.u32(static_cast<std::uint32_t>(membership.learners.size()));
    for (const consensus::NodeId& id : membership.learners) {
        writer.str(id);
    }
}

void encodeConfig(ByteWriter& writer, const consensus::Configuration& conf) {
    encodeMembership(writer, conf.incoming);
    encodeMembership(writer, conf.outgoing);
}

bool decodeMembership(ByteReader& reader, consensus::Membership& membership) {
    std::uint32_t count = 0;
    if (!reader.u32(count) || count > kMaxMembers) {
        return false;
    }
    membership.voters.clear();
    membership.voters.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        consensus::NodeId id;
        if (!reader.str(id, kMaxIdBytes)) {
            return false;
        }
        membership.voters.push_back(std::move(id));
    }
    if (!reader.u32(count) || count > kMaxMembers) {
        return false;
    }
    membership.learners.clear();
    membership.learners.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        consensus::NodeId id;
        if (!reader.str(id, kMaxIdBytes)) {
            return false;
        }
        membership.learners.push_back(std::move(id));
    }
    return true;
}

bool decodeConfig(ByteReader& reader, consensus::Configuration& conf) {
    return decodeMembership(reader, conf.incoming) &&
           decodeMembership(reader, conf.outgoing);
}

std::string encodeEntryRecord(const consensus::LogEntry& entry) {
    ByteWriter body;
    body.u8(kRecordEntry);
    body.u64(entry.term);
    body.u64(entry.index);
    body.u8(static_cast<std::uint8_t>(entry.type));
    body.str(entry.payload);
    encodeConfig(body, entry.config);
    const std::uint32_t crc = crc32Ieee(body.data());

    ByteWriter record;
    record.u32(static_cast<std::uint32_t>(body.size() + 4));
    record.raw(body.data());
    record.u32(crc);
    return record.finish();
}

bool decodeEntryRecord(std::string_view body, consensus::LogEntry& entry, std::string& error) {
    if (body.size() < 4) {
        error = "WAL record is too short";
        return false;
    }
    const std::string_view payload(body.data(), body.size() - 4);
    ByteReader crc_reader(std::string_view(body.data() + body.size() - 4, 4));
    std::uint32_t stored = 0;
    if (!crc_reader.u32(stored) || stored != crc32Ieee(payload)) {
        error = "WAL checksum mismatch";
        return false;
    }
    ByteReader reader(payload);
    std::uint8_t type = 0;
    std::uint8_t entry_type = 0;
    if (!reader.u8(type) || type != kRecordEntry || !reader.u64(entry.term) ||
        !reader.u64(entry.index) || !reader.u8(entry_type) ||
        !reader.str(entry.payload) || !decodeConfig(reader, entry.config) ||
        !reader.done()) {
        error = "WAL record is truncated or trailing";
        return false;
    }
    if (entry_type < static_cast<std::uint8_t>(consensus::EntryType::kCommand) ||
        entry_type > static_cast<std::uint8_t>(consensus::EntryType::kConfig)) {
        error = "WAL record has an unknown entry type";
        return false;
    }
    entry.type = static_cast<consensus::EntryType>(entry_type);
    if (entry.index == 0) {
        error = "WAL record index must be greater than zero";
        return false;
    }
    return true;
}

int openCloexec(const char* path, int flags) {
    int fd = -1;
    do {
#ifdef O_CLOEXEC
        fd = open(path, flags | O_CLOEXEC);
#else
        fd = open(path, flags);
#endif
    } while (fd == -1 && errno == EINTR);
    return fd;
}

bool parseSegmentName(const std::string& name, std::uint64_t& seq) {
    unsigned long long value = 0;
    if (std::sscanf(name.c_str(), "segment-%016llu.wal", &value) != 1) {
        return false;
    }
    seq = static_cast<std::uint64_t>(value);
    return true;
}

bool removeSnapshotDirectory(const std::string& path, std::string& error) {
    const std::string manifest = joinPath(path, "manifest");
    const std::string payload = joinPath(path, "payload");
    if (!removeFile(manifest, error) || !removeFile(payload, error)) {
        return false;
    }
    if (rmdir(path.c_str()) == -1 && errno != ENOENT) {
        error = errnoMessage("cannot remove snapshot directory", path);
        return false;
    }
    return true;
}

}  // namespace

struct PersistentStorage::Impl {
    OpenOptions options;
    std::string last_error;
    bool failed = false;
    IoStats stats;
    consensus::HardState hard_state;
    consensus::Configuration conf;
    consensus::Snapshot snapshot;
    std::vector<consensus::LogEntry> entries;
    std::map<std::string, consensus::Index> pins;

    struct Segment {
        std::uint64_t seq = 0;
        std::string path;
        std::uint16_t header_size = 0;
        consensus::Index first_index = 0;
        consensus::Index last_index = 0;
        std::uint64_t size = 0;
    };
    std::vector<Segment> segments;

    int active_fd = -1;
    std::uint64_t active_seq = 0;
    std::uint64_t active_size = 0;
    std::uint16_t active_header = 0;
    bool dirty = false;
    std::uint64_t next_seq = 1;

    ~Impl() { closeActive(); }

    bool fail(std::string message) {
        last_error = std::move(message);
        failed = true;
        return false;
    }

    bool checkOpen() {
        if (failed) {
            if (last_error.empty()) {
                last_error = "persistent storage is closed after a durability failure";
            }
            return false;
        }
        return true;
    }

    std::string identityPath() const { return joinPath(options.data_dir, "identity"); }
    std::string groupsDir() const { return joinPath(options.data_dir, "groups"); }
    std::string groupPath() const {
        return joinPath(groupsDir(), std::to_string(options.group_id));
    }
    std::string walDir() const { return joinPath(groupPath(), "wal"); }
    std::string snapshotsDir() const { return joinPath(groupPath(), "snapshots"); }
    std::string tmpDir() const { return joinPath(groupPath(), "tmp"); }
    std::string hardStatePath() const { return joinPath(groupPath(), "hard-state"); }
    std::string currentPath() const { return joinPath(groupPath(), "CURRENT"); }

    std::string segmentName(std::uint64_t seq) const {
        return "segment-" + padU64(seq) + ".wal";
    }

    std::string snapshotDirName(consensus::Index index, consensus::Term term) const {
        return padU64(index) + "-" + padU64(term);
    }

    void closeActive() {
        if (active_fd != -1) {
            close(active_fd);
            active_fd = -1;
        }
        dirty = false;
    }

    bool initialize(std::string& error) {
        if (options.data_dir.empty() || options.cluster_id.empty() ||
            options.node_id.empty()) {
            error = "data_dir, cluster_id and node_id are required";
            return false;
        }
        if (options.segment_bytes < 128) {
            error = "WAL segment size is too small";
            return false;
        }
        if (!createDirectories(walDir(), error) ||
            !createDirectories(snapshotsDir(), error) ||
            !createDirectories(tmpDir(), error)) {
            return false;
        }
        if (!loadOrCreateIdentity(error) || !loadSnapshot(error) ||
            !loadHardState(error) || !loadWal(error) || !verifyCommittedRange(error)) {
            return false;
        }
        conf = lastConfigFromLog();
        return true;
    }

    bool loadOrCreateIdentity(std::string& error) {
        if (!fileExists(identityPath())) {
            ByteWriter writer;
            writer.magic(kIdentityMagic);
            writer.u16(kFormatVersion);
            writer.str(options.cluster_id);
            writer.str(options.node_id);
            appendCrc(writer);
            if (!writeFileAtomically(identityPath(), writer.finish(), error)) {
                return false;
            }
            ++stats.atomic_writes;
            return true;
        }
        std::string bytes;
        if (!readFile(identityPath(), bytes, kMaxIdentityBytes, error) ||
            !checkCrc(bytes, error, "identity")) {
            return false;
        }
        ByteReader reader(std::string_view(bytes.data(), bytes.size() - 4));
        std::uint16_t version = 0;
        std::string cluster_id;
        std::string node_id;
        if (!reader.magic(kIdentityMagic) || !reader.u16(version) ||
            version != kFormatVersion || !reader.str(cluster_id, kMaxIdBytes) ||
            !reader.str(node_id, kMaxIdBytes) || !reader.done()) {
            error = "invalid identity file";
            return false;
        }
        if (cluster_id != options.cluster_id || node_id != options.node_id) {
            error = "identity mismatch: directory belongs to cluster '" + cluster_id +
                    "' node '" + node_id + "'";
            return false;
        }
        return true;
    }

    bool identityMatches(const std::string& cluster_id, const std::string& node_id,
                         consensus::GroupId group_id, std::string& error) const {
        if (cluster_id != options.cluster_id || node_id != options.node_id ||
            group_id != options.group_id) {
            error = "Node, Cluster or Group identity mismatch";
            return false;
        }
        return true;
    }

    bool loadSnapshot(std::string& error) {
        if (!fileExists(currentPath())) {
            return true;
        }
        std::string bytes;
        if (!readFile(currentPath(), bytes, kMaxManifestBytes, error) ||
            !checkCrc(bytes, error, "CURRENT")) {
            return false;
        }
        ByteReader reader(std::string_view(bytes.data(), bytes.size() - 4));
        std::uint16_t version = 0;
        std::uint64_t index = 0;
        std::uint64_t term = 0;
        std::string dirname;
        if (!reader.magic(kCurrentMagic) || !reader.u16(version) ||
            version != kFormatVersion || !reader.u64(index) || !reader.u64(term) ||
            !reader.str(dirname, 128) || !reader.done()) {
            error = "invalid CURRENT snapshot pointer";
            return false;
        }
        const std::string dir = joinPath(snapshotsDir(), dirname);
        std::string manifest;
        std::string payload;
        if (!readFile(joinPath(dir, "manifest"), manifest, kMaxManifestBytes, error) ||
            !checkCrc(manifest, error, "snapshot manifest") ||
            !readFile(joinPath(dir, "payload"), payload, kMaxPayloadBytes, error)) {
            return false;
        }
        ByteReader body(std::string_view(manifest.data(), manifest.size() - 4));
        std::string cluster_id;
        std::string node_id;
        std::uint64_t group = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_term = 0;
        std::uint32_t payload_crc = 0;
        std::uint64_t payload_size = 0;
        consensus::Configuration snap_conf;
        if (!body.magic(kManifestMagic) || !body.u16(version) || version != kFormatVersion ||
            !body.str(cluster_id, kMaxIdBytes) || !body.str(node_id, kMaxIdBytes) ||
            !body.u64(group) || !body.u64(last_index) || !body.u64(last_term) ||
            !decodeConfig(body, snap_conf) || !body.u32(payload_crc) ||
            !body.u64(payload_size) || !body.done()) {
            error = "invalid snapshot manifest";
            return false;
        }
        if (!identityMatches(cluster_id, node_id, group, error)) {
            return false;
        }
        if (last_index != index || last_term != term) {
            error = "CURRENT snapshot pointer does not match the manifest";
            return false;
        }
        if (payload.size() != payload_size || crc32Ieee(payload) != payload_crc) {
            error = "snapshot payload checksum mismatch";
            return false;
        }
        snapshot.last_included_index = last_index;
        snapshot.last_included_term = last_term;
        snapshot.conf = consensus::normalizeConf(std::move(snap_conf));
        snapshot.data = std::move(payload);
        return true;
    }

    bool loadHardState(std::string& error) {
        if (!fileExists(hardStatePath())) {
            if (snapshot.last_included_index > hard_state.commit_index) {
                hard_state.commit_index = snapshot.last_included_index;
            }
            return true;
        }
        std::string bytes;
        if (!readFile(hardStatePath(), bytes, kMaxHardStateBytes, error) ||
            !checkCrc(bytes, error, "hard-state")) {
            return false;
        }
        ByteReader reader(std::string_view(bytes.data(), bytes.size() - 4));
        std::uint16_t version = 0;
        std::string cluster_id;
        std::string node_id;
        std::uint64_t group = 0;
        if (!reader.magic(kHardStateMagic) || !reader.u16(version) ||
            version != kFormatVersion || !reader.str(cluster_id, kMaxIdBytes) ||
            !reader.str(node_id, kMaxIdBytes) || !reader.u64(group) ||
            !reader.u64(hard_state.current_term) ||
            !reader.str(hard_state.voted_for, kMaxIdBytes) ||
            !reader.u64(hard_state.commit_index) || !reader.done()) {
            error = "invalid hard-state file";
            return false;
        }
        if (!identityMatches(cluster_id, node_id, group, error)) {
            return false;
        }
        if (snapshot.last_included_index > hard_state.commit_index) {
            hard_state.commit_index = snapshot.last_included_index;
        }
        return true;
    }

    bool loadWal(std::string& error) {
        std::string list_error;
        std::vector<std::string> names = listDirectory(walDir(), list_error);
        if (!list_error.empty()) {
            error = list_error;
            return false;
        }
        std::vector<std::pair<std::uint64_t, std::string>> ordered;
        for (const std::string& name : names) {
            std::uint64_t seq = 0;
            if (!parseSegmentName(name, seq)) {
                continue;
            }
            ordered.emplace_back(seq, joinPath(walDir(), name));
        }
        std::sort(ordered.begin(), ordered.end());
        consensus::Index wal_expected = 0;
        consensus::Term previous_term = 0;
        bool have_wal = false;
        std::vector<consensus::LogEntry> loaded;
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            const bool last_segment = i + 1 == ordered.size();
            Segment segment;
            std::vector<consensus::LogEntry> records;
            if (!readSegment(ordered[i].second, ordered[i].first, last_segment, segment,
                             records, error)) {
                return false;
            }
            for (const consensus::LogEntry& entry : records) {
                if (have_wal && entry.index != wal_expected) {
                    error = "WAL index hole at " + std::to_string(wal_expected);
                    return false;
                }
                if (have_wal && entry.term < previous_term) {
                    error = "WAL term mismatch at index " + std::to_string(entry.index);
                    return false;
                }
                if (snapshot.last_included_index != 0 &&
                    entry.index == snapshot.last_included_index &&
                    entry.term != snapshot.last_included_term) {
                    error = "WAL term mismatch with snapshot at index " +
                            std::to_string(entry.index);
                    return false;
                }
                if (snapshot.last_included_index != 0 &&
                    entry.index == snapshot.last_included_index + 1 &&
                    entry.term < snapshot.last_included_term) {
                    error = "WAL term mismatch at index " + std::to_string(entry.index);
                    return false;
                }
                wal_expected = entry.index + 1;
                previous_term = entry.term;
                have_wal = true;
                loaded.push_back(entry);
            }
            if (segment.seq >= next_seq) {
                next_seq = segment.seq + 1;
            }
            segments.push_back(std::move(segment));
        }
        for (const consensus::LogEntry& entry : loaded) {
            if (entry.index > snapshot.last_included_index) {
                entries.push_back(entry);
            }
        }
        if (!entries.empty() &&
            entries.front().index != snapshot.last_included_index + 1) {
            error = "WAL index hole after snapshot at " +
                    std::to_string(snapshot.last_included_index + 1);
            return false;
        }
        if (!segments.empty() && !openActiveLocked(error)) {
            return false;
        }
        return true;
    }

    bool readSegment(const std::string& path, std::uint64_t seq, bool last_segment,
                     Segment& segment, std::vector<consensus::LogEntry>& records,
                     std::string& error) {
        std::string bytes;
        if (!readFile(path, bytes, kMaxSegmentBytes, error)) {
            return false;
        }
        ByteReader header(bytes);
        std::uint16_t version = 0;
        std::uint16_t header_size = 0;
        std::string cluster_id;
        std::string node_id;
        std::uint64_t group = 0;
        std::uint64_t stored_seq = 0;
        if (!header.magic(kWalMagic) || !header.u16(version) || version != kFormatVersion ||
            !header.u16(header_size) || header_size < 16 || header_size > bytes.size()) {
            error = "invalid WAL segment header in '" + path + "'";
            return false;
        }
        ByteReader body(std::string_view(bytes.data(), header_size));
        if (!body.magic(kWalMagic) || !body.u16(version) || !body.u16(header_size) ||
            !body.str(cluster_id, kMaxIdBytes) || !body.str(node_id, kMaxIdBytes) ||
            !body.u64(group) || !body.u64(stored_seq)) {
            error = "truncated WAL segment header in '" + path + "'";
            return false;
        }
        std::uint32_t stored_crc = 0;
        if (!body.u32(stored_crc) || !body.done()) {
            error = "WAL segment header checksum is missing";
            return false;
        }
        const std::uint32_t actual_crc =
            crc32Ieee(std::string_view(bytes.data(), header_size - 4));
        if (actual_crc != stored_crc) {
            error = "WAL segment header checksum mismatch";
            return false;
        }
        if (!identityMatches(cluster_id, node_id, group, error)) {
            return false;
        }
        if (stored_seq != seq) {
            error = "WAL segment sequence mismatch";
            return false;
        }

        segment.seq = seq;
        segment.path = path;
        segment.header_size = header_size;
        segment.size = bytes.size();
        std::size_t offset = header_size;
        std::size_t good_size = header_size;
        while (offset < bytes.size()) {
            const std::size_t remaining = bytes.size() - offset;
            if (remaining < 4) {
                if (!last_segment) {
                    error = "WAL torn write in the middle of a segment";
                    return false;
                }
                break;
            }
            ByteReader len_reader(std::string_view(bytes.data() + offset, 4));
            std::uint32_t body_len = 0;
            if (!len_reader.u32(body_len) || body_len == 0 || body_len > kMaxRecordBytes) {
                if (!last_segment) {
                    error = "WAL checksum mismatch";
                    return false;
                }
                break;
            }
            if (remaining < 4u + body_len) {
                if (!last_segment) {
                    error = "WAL torn write in the middle of a segment";
                    return false;
                }
                break;
            }
            consensus::LogEntry entry;
            std::string decode_error;
            if (!decodeEntryRecord(std::string_view(bytes.data() + offset + 4, body_len),
                                   entry, decode_error)) {
                const bool at_tail = offset + 4 + body_len == bytes.size();
                if (last_segment && at_tail &&
                    decode_error.find("checksum") != std::string::npos) {
                    break;
                }
                error = decode_error;
                return false;
            }
            if (segment.first_index == 0) {
                segment.first_index = entry.index;
            }
            segment.last_index = entry.index;
            records.push_back(std::move(entry));
            offset += 4u + body_len;
            good_size = offset;
        }
        if (good_size != bytes.size()) {
            const int fd = openCloexec(path.c_str(), O_RDWR);
            if (fd == -1) {
                error = errnoMessage("cannot reopen WAL segment to truncate torn tail", path);
                return false;
            }
            if (ftruncate(fd, static_cast<off_t>(good_size)) == -1 ||
                fsync(fd) == -1) {
                error = errnoMessage("cannot truncate torn WAL tail", path);
                close(fd);
                return false;
            }
            close(fd);
            ++stats.wal_fsyncs;
            segment.size = good_size;
        }
        return true;
    }

    bool verifyCommittedRange(std::string& error) const {
        if (hard_state.commit_index < snapshot.last_included_index) {
            error = "commit index is behind the snapshot";
            return false;
        }
        const consensus::Index last =
            entries.empty() ? snapshot.last_included_index : entries.back().index;
        if (hard_state.commit_index > last) {
            error = "WAL index hole in committed range (" +
                    std::to_string(snapshot.last_included_index) + ", " +
                    std::to_string(hard_state.commit_index) + "]";
            return false;
        }
        consensus::Index expected = snapshot.last_included_index + 1;
        for (const consensus::LogEntry& entry : entries) {
            if (entry.index > hard_state.commit_index) {
                break;
            }
            if (entry.index != expected) {
                error = "WAL index hole at " + std::to_string(expected);
                return false;
            }
            ++expected;
        }
        return true;
    }

    bool openActiveLocked(std::string& error) {
        closeActive();
        if (segments.empty()) {
            return true;
        }
        Segment& segment = segments.back();
        active_fd = openCloexec(segment.path.c_str(), O_RDWR);
        if (active_fd == -1) {
            error = errnoMessage("cannot open WAL segment", segment.path);
            return false;
        }
        active_seq = segment.seq;
        active_size = segment.size;
        active_header = segment.header_size;
        if (lseek(active_fd, static_cast<off_t>(active_size), SEEK_SET) == -1) {
            error = errnoMessage("cannot seek WAL segment", segment.path);
            closeActive();
            return false;
        }
        return true;
    }

    std::string encodeSegmentHeader(std::uint64_t seq) const {
        ByteWriter writer;
        writer.magic(kWalMagic);
        writer.u16(kFormatVersion);
        writer.u16(0);
        writer.str(options.cluster_id);
        writer.str(options.node_id);
        writer.u64(options.group_id);
        writer.u64(seq);
        const std::uint16_t header_size =
            static_cast<std::uint16_t>(writer.size() + 4);
        std::string bytes = writer.finish();
        bytes[6] = static_cast<char>((header_size >> 8) & 0xff);
        bytes[7] = static_cast<char>(header_size & 0xff);
        ByteWriter out;
        out.raw(bytes);
        appendCrc(out);
        return out.finish();
    }

    bool createSegment(std::string& error) {
        closeActive();
        const std::uint64_t seq = next_seq++;
        const std::string path = joinPath(walDir(), segmentName(seq));
        const std::string header = encodeSegmentHeader(seq);
        if (!writeFileAtomically(path, header, error)) {
            return false;
        }
        ++stats.atomic_writes;
        Segment segment;
        segment.seq = seq;
        segment.path = path;
        segment.header_size = static_cast<std::uint16_t>(header.size());
        segment.size = header.size();
        segments.push_back(segment);
        return openActiveLocked(error);
    }

    bool writeFully(const std::string& bytes, std::string& error) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t n =
                write(active_fd, bytes.data() + offset, bytes.size() - offset);
            if (n > 0) {
                offset += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            error = errnoMessage("cannot append WAL record", segments.back().path);
            return false;
        }
        active_size += bytes.size();
        segments.back().size = active_size;
        dirty = true;
        return true;
    }

    bool syncWal(std::string& error) {
        if (active_fd == -1 || !dirty) {
            return true;
        }
        if (!fsyncFd(active_fd, error)) {
            error = errnoMessage("cannot fsync WAL segment", segments.back().path);
            return false;
        }
        dirty = false;
        ++stats.wal_fsyncs;
        return true;
    }

    bool rollIfNeeded(std::size_t record_size, std::string& error) {
        if (active_fd == -1) {
            return createSegment(error);
        }
        if (active_size > active_header &&
            active_size + record_size > options.segment_bytes) {
            if (!syncWal(error) || !createSegment(error)) {
                return false;
            }
        }
        return true;
    }

    consensus::Configuration lastConfigFromLog() const {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->type == consensus::EntryType::kConfig) {
                return consensus::normalizeConf(it->config);
            }
        }
        if (snapshot.last_included_index != 0) {
            return consensus::normalizeConf(snapshot.conf);
        }
        return consensus::normalizeConf(options.bootstrap);
    }

    consensus::Index lastIndex() const {
        if (!entries.empty()) {
            return entries.back().index;
        }
        return snapshot.last_included_index;
    }

    consensus::Index recycleThroughIndex() const {
        consensus::Index through = snapshot.last_included_index;
        for (const auto& pin : pins) {
            through = std::min(through, pin.second);
        }
        return through;
    }

    bool saveHardStateLocked(const consensus::HardState& hs, std::string& error) {
        ByteWriter writer;
        writer.magic(kHardStateMagic);
        writer.u16(kFormatVersion);
        writer.str(options.cluster_id);
        writer.str(options.node_id);
        writer.u64(options.group_id);
        writer.u64(hs.current_term);
        writer.str(hs.voted_for);
        writer.u64(hs.commit_index);
        appendCrc(writer);
        if (!writeFileAtomically(hardStatePath(), writer.finish(), error)) {
            return false;
        }
        ++stats.atomic_writes;
        hard_state = hs;
        if (snapshot.last_included_index > hard_state.commit_index) {
            hard_state.commit_index = snapshot.last_included_index;
        }
        return true;
    }

    bool truncateFromLocked(consensus::Index index, std::string& error) {
        if (index == 0) {
            return fail("cannot truncate from index 0");
        }
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [index](const consensus::LogEntry& entry) {
                                         return entry.index >= index;
                                     }),
                      entries.end());
        if (segments.empty()) {
            conf = lastConfigFromLog();
            return true;
        }

        std::size_t delete_from = segments.size();
        std::uint64_t cut_size = 0;
        consensus::Index kept_first = 0;
        consensus::Index kept_last = 0;
        bool truncate_live = false;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (segments[i].last_index != 0 && segments[i].last_index < index) {
                continue;
            }
            std::string bytes;
            if (!readFile(segments[i].path, bytes, kMaxSegmentBytes, error)) {
                return false;
            }
            std::size_t offset = segments[i].header_size;
            std::size_t keep_end = offset;
            consensus::Index first = 0;
            consensus::Index last = 0;
            while (offset + 4 <= bytes.size()) {
                ByteReader len_reader(std::string_view(bytes.data() + offset, 4));
                std::uint32_t body_len = 0;
                if (!len_reader.u32(body_len) || offset + 4 + body_len > bytes.size()) {
                    break;
                }
                consensus::LogEntry entry;
                std::string decode_error;
                if (!decodeEntryRecord(std::string_view(bytes.data() + offset + 4, body_len),
                                       entry, decode_error)) {
                    error = decode_error;
                    return false;
                }
                if (entry.index >= index) {
                    break;
                }
                if (first == 0) {
                    first = entry.index;
                }
                last = entry.index;
                offset += 4u + body_len;
                keep_end = offset;
            }
            if (keep_end == segments[i].header_size) {
                delete_from = i;
                truncate_live = false;
            } else {
                delete_from = i + 1;
                cut_size = keep_end;
                kept_first = first;
                kept_last = last;
                truncate_live = keep_end < segments[i].size;
            }
            break;
        }

        closeActive();
        if (truncate_live && delete_from > 0) {
            Segment& live = segments[delete_from - 1];
            live.size = cut_size;
            live.first_index = kept_first;
            live.last_index = kept_last;
            const int fd = openCloexec(live.path.c_str(), O_RDWR);
            if (fd == -1) {
                error = errnoMessage("cannot reopen WAL segment for truncate", live.path);
                return false;
            }
            if (ftruncate(fd, static_cast<off_t>(live.size)) == -1 || fsync(fd) == -1) {
                error = errnoMessage("cannot truncate WAL segment", live.path);
                close(fd);
                return false;
            }
            close(fd);
            ++stats.wal_fsyncs;
        }
        for (std::size_t i = delete_from; i < segments.size(); ++i) {
            if (!removeFile(segments[i].path, error)) {
                return false;
            }
        }
        segments.resize(delete_from);
        if (!fsyncDirectory(walDir(), error) || !openActiveLocked(error)) {
            return false;
        }
        conf = lastConfigFromLog();
        return true;
    }

    bool appendLocked(const std::vector<consensus::LogEntry>& ents, std::string& error) {
        for (const consensus::LogEntry& entry : ents) {
            if (entry.index == 0) {
                return fail("cannot append a WAL entry at index 0");
            }
            if (!entries.empty() && entry.index <= entries.back().index) {
                if (!truncateFromLocked(entry.index, error)) {
                    return false;
                }
            }
            const std::string record = encodeEntryRecord(entry);
            if (!rollIfNeeded(record.size(), error) || !writeFully(record, error)) {
                return false;
            }
            Segment& segment = segments.back();
            if (segment.first_index == 0) {
                segment.first_index = entry.index;
            }
            segment.last_index = entry.index;
            entries.push_back(entry);
            if (entry.type == consensus::EntryType::kConfig) {
                conf = consensus::normalizeConf(entry.config);
            }
            ++stats.records_appended;
        }
        return true;
    }

    bool writeCurrent(consensus::Index index, consensus::Term term,
                      const std::string& dirname, std::string& error) {
        ByteWriter writer;
        writer.magic(kCurrentMagic);
        writer.u16(kFormatVersion);
        writer.u64(index);
        writer.u64(term);
        writer.str(dirname);
        appendCrc(writer);
        if (!writeFileAtomically(currentPath(), writer.finish(), error)) {
            return false;
        }
        ++stats.atomic_writes;
        return true;
    }

    bool installSnapshotLocked(const consensus::Snapshot& snap, std::string& error) {
        if (snap.last_included_index < snapshot.last_included_index) {
            return fail("cannot install an older snapshot");
        }
        const std::string dirname =
            snapshotDirName(snap.last_included_index, snap.last_included_term);
        const std::string final_dir = joinPath(snapshotsDir(), dirname);
        if (!(fileExists(joinPath(final_dir, "manifest")) &&
              snap.last_included_index == snapshot.last_included_index &&
              snap.last_included_term == snapshot.last_included_term)) {
            std::string tmpl = joinPath(tmpDir(), "snap.XXXXXX");
            std::vector<char> path(tmpl.begin(), tmpl.end());
            path.push_back('\0');
            if (mkdtemp(path.data()) == nullptr) {
                error = errnoMessage("cannot create temporary snapshot directory", tmpDir());
                return false;
            }
            const std::string tmp(path.data());
            ByteWriter manifest;
            manifest.magic(kManifestMagic);
            manifest.u16(kFormatVersion);
            manifest.str(options.cluster_id);
            manifest.str(options.node_id);
            manifest.u64(options.group_id);
            manifest.u64(snap.last_included_index);
            manifest.u64(snap.last_included_term);
            encodeConfig(manifest, snap.conf);
            manifest.u32(crc32Ieee(snap.data));
            manifest.u64(snap.data.size());
            appendCrc(manifest);
            if (!writeFileAtomically(joinPath(tmp, "payload"), snap.data, error) ||
                !writeFileAtomically(joinPath(tmp, "manifest"), manifest.finish(), error)) {
                removeSnapshotDirectory(tmp, error);
                return false;
            }
            stats.atomic_writes += 2;
            if (directoryExists(final_dir) &&
                !removeSnapshotDirectory(final_dir, error)) {
                removeSnapshotDirectory(tmp, error);
                return false;
            }
            if (rename(tmp.c_str(), final_dir.c_str()) == -1) {
                error = errnoMessage("cannot activate snapshot directory", final_dir);
                removeSnapshotDirectory(tmp, error);
                return false;
            }
            if (!fsyncDirectory(snapshotsDir(), error)) {
                return false;
            }
        }
        if (!writeCurrent(snap.last_included_index, snap.last_included_term, dirname,
                          error)) {
            return false;
        }

        snapshot = snap;
        snapshot.conf = consensus::normalizeConf(snapshot.conf);
        if (hard_state.commit_index < snapshot.last_included_index) {
            hard_state.commit_index = snapshot.last_included_index;
        }
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const consensus::LogEntry& entry) {
                                         return entry.index <= snapshot.last_included_index;
                                     }),
                      entries.end());
        conf = lastConfigFromLog();
        if (!recycleWal(error)) {
            return false;
        }
        return true;
    }

    bool recycleWal(std::string& error) {
        const consensus::Index through = recycleThroughIndex();
        if (through == 0 || segments.empty()) {
            return true;
        }
        std::size_t keep_from = 0;
        while (keep_from < segments.size() && segments[keep_from].last_index != 0 &&
               segments[keep_from].last_index <= through) {
            ++keep_from;
        }
        if (keep_from == 0) {
            return true;
        }
        closeActive();
        for (std::size_t i = 0; i < keep_from; ++i) {
            if (!removeFile(segments[i].path, error)) {
                return false;
            }
        }
        segments.erase(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(keep_from));
        if (!fsyncDirectory(walDir(), error)) {
            return false;
        }
        return openActiveLocked(error);
    }

    bool applyReady(const consensus::Ready& ready) {
        if (!checkOpen()) {
            return false;
        }
        std::string error;
        if (ready.snapshot.has_value() &&
            !installSnapshotLocked(*ready.snapshot, error)) {
            return fail(std::move(error));
        }
        if (ready.truncate_from.has_value() &&
            !truncateFromLocked(*ready.truncate_from, error)) {
            return fail(std::move(error));
        }
        if (!ready.entries.empty() && !appendLocked(ready.entries, error)) {
            return fail(std::move(error));
        }
        if (!syncWal(error)) {
            return fail(std::move(error));
        }
        if (ready.hard_state.has_value() &&
            !saveHardStateLocked(*ready.hard_state, error)) {
            return fail(std::move(error));
        }
        return true;
    }
};

PersistentStorage::PersistentStorage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PersistentStorage::~PersistentStorage() = default;

std::unique_ptr<PersistentStorage> PersistentStorage::open(const OpenOptions& options,
                                                           std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->conf = consensus::normalizeConf(options.bootstrap);
    if (!impl->initialize(error)) {
        return nullptr;
    }
    return std::unique_ptr<PersistentStorage>(new PersistentStorage(std::move(impl)));
}

bool PersistentStorage::saveHardState(const consensus::HardState& hs) {
    if (!impl_->checkOpen()) {
        return false;
    }
    std::string error;
    if (!impl_->saveHardStateLocked(hs, error)) {
        return impl_->fail(std::move(error));
    }
    return true;
}

bool PersistentStorage::truncateFrom(consensus::Index index) {
    if (!impl_->checkOpen()) {
        return false;
    }
    std::string error;
    if (!impl_->truncateFromLocked(index, error)) {
        return impl_->fail(std::move(error));
    }
    return true;
}

bool PersistentStorage::append(const std::vector<consensus::LogEntry>& ents) {
    if (!impl_->checkOpen()) {
        return false;
    }
    std::string error;
    if (!impl_->appendLocked(ents, error) || !impl_->syncWal(error)) {
        return impl_->fail(std::move(error));
    }
    return true;
}

bool PersistentStorage::installSnapshot(const consensus::Snapshot& snapshot) {
    if (!impl_->checkOpen()) {
        return false;
    }
    std::string error;
    if (!impl_->installSnapshotLocked(snapshot, error)) {
        return impl_->fail(std::move(error));
    }
    return true;
}

bool PersistentStorage::applyReady(const consensus::Ready& ready) {
    return impl_->applyReady(ready);
}

void PersistentStorage::pinWalAfter(const std::string& pin_id, consensus::Index index) {
    impl_->pins[pin_id] = index;
}

void PersistentStorage::unpinWal(const std::string& pin_id) {
    impl_->pins.erase(pin_id);
}

consensus::HardState PersistentStorage::hardState() const { return impl_->hard_state; }

consensus::RaftRestore PersistentStorage::toRestore() const {
    consensus::RaftRestore restore;
    restore.hard_state = impl_->hard_state;
    restore.conf = impl_->conf;
    restore.snapshot_conf = impl_->snapshot.last_included_index == 0
                                ? consensus::Configuration{}
                                : impl_->snapshot.conf;
    restore.snapshot_index = impl_->snapshot.last_included_index;
    restore.snapshot_term = impl_->snapshot.last_included_term;
    restore.snapshot_data = impl_->snapshot.data;
    restore.entries = impl_->entries;
    return restore;
}

const consensus::Snapshot& PersistentStorage::snapshot() const { return impl_->snapshot; }

const std::vector<consensus::LogEntry>& PersistentStorage::entries() const {
    return impl_->entries;
}

std::vector<consensus::LogEntry> PersistentStorage::committedReplay() const {
    std::vector<consensus::LogEntry> out;
    const consensus::Index from = impl_->snapshot.last_included_index;
    const consensus::Index to = impl_->hard_state.commit_index;
    for (const consensus::LogEntry& entry : impl_->entries) {
        if (entry.index > from && entry.index <= to) {
            out.push_back(entry);
        }
    }
    return out;
}

consensus::Configuration PersistentStorage::latestConf() const { return impl_->conf; }

consensus::Index PersistentStorage::lastIndex() const { return impl_->lastIndex(); }

consensus::Index PersistentStorage::recycleThroughIndex() const {
    return impl_->recycleThroughIndex();
}

const IoStats& PersistentStorage::stats() const { return impl_->stats; }

const std::string& PersistentStorage::lastError() const { return impl_->last_error; }

std::string PersistentStorage::groupDir() const { return impl_->groupPath(); }

std::vector<std::string> PersistentStorage::walSegmentPaths() const {
    std::vector<std::string> paths;
    paths.reserve(impl_->segments.size());
    for (const Impl::Segment& segment : impl_->segments) {
        paths.push_back(segment.path);
    }
    return paths;
}

std::size_t PersistentStorage::walSegmentCount() const { return impl_->segments.size(); }

}  // namespace persistence
