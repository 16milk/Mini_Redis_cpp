#include "mini_redis/cluster/SlotMigration.hpp"

#include "mini_redis/cluster/Slot.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/core/RedisObject.hpp"
#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace cluster {
namespace {

constexpr char kCommandMagic[] = {'M', 'R', 'S', 'M'};
constexpr char kProofMagic[] = {'M', 'R', 'C', 'P'};
constexpr char kActivationMagic[] = {'M', 'R', 'A', 'P'};
constexpr char kSnapshotMagic[] = {'M', 'R', 'M', 'G'};
constexpr std::size_t kMaxEncodedBytes = 256U << 20;
constexpr std::size_t kMaxItems = 1U << 22;

// --- byte helpers -----------------------------------------------------------

class Writer {
public:
    void number(std::uint64_t value, unsigned width) {
        for (unsigned shift = width * 8; shift != 0; shift -= 8) {
            bytes_.push_back(static_cast<char>((value >> (shift - 8)) & 0xffU));
        }
    }
    void magic(const char (&value)[4]) { bytes_.append(value, sizeof(value)); }
    void string(const std::string& value) {
        number(value.size(), 4);
        bytes_.append(value);
    }
    void record(const SlotDataRecord& value) {
        number(value.sequence, 8);
        string(value.key);
        number(value.present ? 1U : 0U, 1);
        string(value.value);
        number(static_cast<std::uint64_t>(value.expire_at_ms), 8);
    }
    std::size_t size() const { return bytes_.size(); }
    std::string finish() { return std::move(bytes_); }

private:
    std::string bytes_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool number(unsigned width, std::uint64_t& value) {
        if (offset_ > bytes_.size() || width > bytes_.size() - offset_) return false;
        value = 0;
        for (unsigned index = 0; index < width; ++index) {
            value = (value << 8) | static_cast<unsigned char>(bytes_[offset_++]);
        }
        return true;
    }
    bool magic(const char (&expected)[4]) {
        if (remaining() < sizeof(expected)) return false;
        const bool matches =
            std::memcmp(bytes_.data() + offset_, expected, sizeof(expected)) == 0;
        offset_ += sizeof(expected);
        return matches;
    }
    std::size_t remaining() const {
        return offset_ > bytes_.size() ? 0 : bytes_.size() - offset_;
    }
    // A record cannot be shorter than its fixed-width fields, so a corrupt
    // count is rejected before it can drive a large allocation.
    bool plausibleRecordCount(std::uint64_t count) const {
        constexpr std::size_t kMinRecordBytes = 25;
        return count <= kMaxItems && count <= remaining() / kMinRecordBytes;
    }
    bool string(std::string& value) {
        std::uint64_t size = 0;
        if (!number(4, size) || size > bytes_.size() - offset_) return false;
        value.assign(bytes_.data() + offset_, static_cast<std::size_t>(size));
        offset_ += static_cast<std::size_t>(size);
        return true;
    }
    bool record(SlotDataRecord& value) {
        std::uint64_t flag = 0;
        std::uint64_t deadline = 0;
        if (!number(8, value.sequence) || !string(value.key) || !number(1, flag) ||
            flag > 1 || !string(value.value) || !number(8, deadline)) {
            return false;
        }
        value.present = flag == 1;
        value.expire_at_ms = static_cast<UnixMillis>(deadline);
        return value.present || value.value.empty();
    }
    bool done() const { return offset_ == bytes_.size(); }

private:
    std::string_view bytes_;
    std::size_t offset_ = 0;
};

std::uint64_t doubleToBits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double bitsToDouble(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- value codec ------------------------------------------------------------

enum class ValueKind : std::uint8_t {
    kString = 1,
    kList = 2,
    kSet = 3,
    kHash = 4,
    kZSet = 5,
};

// Collections are emitted in sorted order even where the in-memory encoding is
// unordered, so two replicas of the same shard produce byte-identical records
// and therefore the same digest.
std::string encodeValue(const RedisObject& object) {
    Writer writer;
    switch (object.type()) {
        case ObjectType::STRING: {
            writer.number(static_cast<std::uint8_t>(ValueKind::kString), 1);
            writer.string(static_cast<const StringObject&>(object).value());
            break;
        }
        case ObjectType::LIST: {
            writer.number(static_cast<std::uint8_t>(ValueKind::kList), 1);
            const std::vector<std::string> values =
                static_cast<const ListObject&>(object).values();
            writer.number(values.size(), 4);
            for (const std::string& value : values) writer.string(value);
            break;
        }
        case ObjectType::SET: {
            writer.number(static_cast<std::uint8_t>(ValueKind::kSet), 1);
            std::vector<std::string> members =
                static_cast<const SetObject&>(object).members();
            std::sort(members.begin(), members.end());
            writer.number(members.size(), 4);
            for (const std::string& member : members) writer.string(member);
            break;
        }
        case ObjectType::HASH: {
            writer.number(static_cast<std::uint8_t>(ValueKind::kHash), 1);
            std::vector<std::pair<std::string, std::string>> fields =
                static_cast<const HashObject&>(object).get_all_fields();
            std::sort(fields.begin(), fields.end());
            writer.number(fields.size(), 4);
            for (const auto& [field, value] : fields) {
                writer.string(field);
                writer.string(value);
            }
            break;
        }
        case ObjectType::ZSET: {
            writer.number(static_cast<std::uint8_t>(ValueKind::kZSet), 1);
            std::vector<std::pair<std::string, double>> members =
                static_cast<const ZSetObject&>(object).members_with_scores();
            // Ordered by score bit pattern, not by numeric value. `<` is not a
            // strict weak ordering once a NaN is involved -- every comparison
            // against it is false, which makes std::sort undefined and can
            // leave two replicas holding different byte sequences for the same
            // zset. Bits are a total order over every double, and they are what
            // gets written out anyway.
            std::sort(members.begin(), members.end(),
                      [](const std::pair<std::string, double>& left,
                         const std::pair<std::string, double>& right) {
                          const std::uint64_t left_bits = doubleToBits(left.second);
                          const std::uint64_t right_bits = doubleToBits(right.second);
                          if (left_bits != right_bits) {
                              return left_bits < right_bits;
                          }
                          return left.first < right.first;
                      });
            writer.number(members.size(), 4);
            for (const auto& [member, score] : members) {
                writer.string(member);
                writer.number(doubleToBits(score), 8);
            }
            break;
        }
    }
    return writer.finish();
}

std::shared_ptr<RedisObject> decodeValue(const std::string& bytes,
                                         std::string& error) {
    Reader reader(bytes);
    std::uint64_t kind = 0;
    std::uint64_t count = 0;
    if (!reader.number(1, kind)) {
        error = "truncated migration value";
        return nullptr;
    }
    std::shared_ptr<RedisObject> object;
    switch (static_cast<ValueKind>(kind)) {
        case ValueKind::kString: {
            std::string value;
            if (!reader.string(value)) break;
            object = std::make_shared<StringObject>(std::move(value));
            break;
        }
        case ValueKind::kList: {
            if (!reader.number(4, count) || count > kMaxItems) break;
            auto list = std::make_shared<ListObject>();
            bool ok = true;
            for (std::uint64_t index = 0; index < count && ok; ++index) {
                std::string value;
                ok = reader.string(value);
                if (ok) list->push_back(std::move(value));
            }
            if (ok) object = std::move(list);
            break;
        }
        case ValueKind::kSet: {
            if (!reader.number(4, count) || count > kMaxItems) break;
            auto set = std::make_shared<SetObject>();
            bool ok = true;
            for (std::uint64_t index = 0; index < count && ok; ++index) {
                std::string member;
                ok = reader.string(member);
                if (ok) set->add(member);
            }
            if (ok) object = std::move(set);
            break;
        }
        case ValueKind::kHash: {
            if (!reader.number(4, count) || count > kMaxItems) break;
            auto hash = std::make_shared<HashObject>();
            bool ok = true;
            for (std::uint64_t index = 0; index < count && ok; ++index) {
                std::string field;
                std::string value;
                ok = reader.string(field) && reader.string(value);
                if (ok) hash->set_field(std::move(field), std::move(value));
            }
            if (ok) object = std::move(hash);
            break;
        }
        case ValueKind::kZSet: {
            if (!reader.number(4, count) || count > kMaxItems) break;
            auto zset = std::make_shared<ZSetObject>();
            bool ok = true;
            for (std::uint64_t index = 0; index < count && ok; ++index) {
                std::string member;
                std::uint64_t score_bits = 0;
                ok = reader.string(member) && reader.number(8, score_bits);
                if (ok) zset->add(bitsToDouble(score_bits), member);
            }
            if (ok) object = std::move(zset);
            break;
        }
        default:
            error = "unknown migration value kind";
            return nullptr;
    }
    if (!object || !reader.done()) {
        error = "truncated or trailing migration value";
        return nullptr;
    }
    error.clear();
    return object;
}

// --- digest -----------------------------------------------------------------

// FNV-1a, 128 bit. The prime is 2^88 + 0x13b, so the multiply reduces to a
// shift plus a multiply by a 9-bit constant and stays in portable C++17.
struct Digest128 {
    std::uint64_t high = 0x6c62272e07bb0142ULL;
    std::uint64_t low = 0x62b821756295c58dULL;
};

void digestByte(Digest128& digest, unsigned char byte) {
    digest.low ^= byte;
    constexpr std::uint64_t kPrimeLow = 0x13bULL;
    const std::uint64_t lower = (digest.low & 0xffffffffULL) * kPrimeLow;
    const std::uint64_t upper = (digest.low >> 32) * kPrimeLow;
    const std::uint64_t shifted = upper << 32;
    const std::uint64_t next_low = lower + shifted;
    std::uint64_t next_high = (upper >> 32) + (next_low < shifted ? 1ULL : 0ULL);
    next_high += digest.high * kPrimeLow;
    next_high += digest.low << 24;  // the 2^88 term
    digest.high = next_high;
    digest.low = next_low;
}

void digestBytes(Digest128& digest, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        digestByte(digest, bytes[index]);
    }
}

void digestNumber(Digest128& digest, std::uint64_t value) {
    for (unsigned shift = 64; shift != 0; shift -= 8) {
        digestByte(digest, static_cast<unsigned char>((value >> (shift - 8)) & 0xffU));
    }
}

void digestString(Digest128& digest, const std::string& value) {
    digestNumber(digest, value.size());
    digestBytes(digest, value.data(), value.size());
}

std::string toHex(const Digest128& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (std::uint64_t word : {digest.high, digest.low}) {
        for (unsigned shift = 64; shift != 0; shift -= 4) {
            out.push_back(kHex[(word >> (shift - 4)) & 0xfU]);
        }
    }
    return out;
}

bool expiredAt(const SlotDataRecord& record, UnixMillis logical_time_ms) {
    return record.expire_at_ms != 0 && record.expire_at_ms <= logical_time_ms;
}

MigrationApplyResult fail(MigrationApplyStatus status, std::string error) {
    MigrationApplyResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

MigrationApplyResult ok(MigrationApplyStatus status = MigrationApplyStatus::kApplied) {
    MigrationApplyResult result;
    result.status = status;
    return result;
}

} // namespace

// --- digest and slot export -------------------------------------------------

std::string slotDigest(const std::vector<SlotDataRecord>& records,
                       UnixMillis logical_time_ms) {
    std::vector<const SlotDataRecord*> live;
    live.reserve(records.size());
    for (const SlotDataRecord& record : records) {
        if (record.present && !expiredAt(record, logical_time_ms)) {
            live.push_back(&record);
        }
    }
    std::sort(live.begin(), live.end(),
              [](const SlotDataRecord* left, const SlotDataRecord* right) {
                  return left->key < right->key;
              });

    Digest128 digest;
    digestNumber(digest, live.size());
    for (const SlotDataRecord* record : live) {
        digestString(digest, record->key);
        digestString(digest, record->value);
        digestNumber(digest, static_cast<std::uint64_t>(record->expire_at_ms));
    }
    return toHex(digest);
}

std::vector<SlotDataRecord> exportSlotRecords(const Database& database, SlotId slot,
                                              UnixMillis logical_time_ms) {
    const std::vector<StoredEntry> entries =
        database.exportKeys([slot](const std::string& key) {
            return keyToSlot(key) == slot;
        });
    std::vector<SlotDataRecord> records;
    records.reserve(entries.size());
    for (const StoredEntry& entry : entries) {
        if (!entry.object) continue;
        if (entry.expire_at_ms != 0 && entry.expire_at_ms <= logical_time_ms) {
            continue;  // already logically dead; the target must not receive it
        }
        SlotDataRecord record;
        record.key = entry.key;
        record.present = true;
        record.value = encodeValue(*entry.object);
        record.expire_at_ms = entry.expire_at_ms;
        records.push_back(std::move(record));
    }
    return records;
}

// --- commit proof -----------------------------------------------------------

namespace {

// Strips and checks the trailing tag. A proof that fails here never becomes a
// decoded struct, so no caller can accidentally act on unauthenticated fields.
bool unwrapSignature(const std::string& encoded, const MigrationKey& key,
                     std::string& body, std::string& error) {
    if (!key.configured()) {
        error = "no migration signing key is configured on this shard";
        return false;
    }
    if (encoded.size() > kMaxEncodedBytes || encoded.size() <= kMigrationTagBytes) {
        error = "migration proof is truncated";
        return false;
    }
    const std::size_t split = encoded.size() - kMigrationTagBytes;
    if (!key.verify(encoded.substr(0, split), encoded.substr(split))) {
        error = "migration proof signature does not verify";
        return false;
    }
    body = encoded.substr(0, split);
    return true;
}

}  // namespace

std::string encodeCommitProof(const MigrationCommitProof& proof,
                              const MigrationKey& key) {
    Writer writer;
    writer.magic(kProofMagic);
    writer.number(kSlotMigrationVersion, 2);
    writer.string(proof.migration_id);
    writer.number(proof.slot, 2);
    writer.number(proof.source, 4);
    writer.number(proof.target, 4);
    writer.number(proof.from_epoch, 8);
    writer.number(proof.to_epoch, 8);
    writer.number(proof.fence_term, 8);
    writer.number(proof.fence_index, 8);
    writer.number(proof.read_index, 8);
    writer.number(proof.fence_sequence, 8);
    writer.number(static_cast<std::uint64_t>(proof.logical_time_ms), 8);
    writer.string(proof.digest);
    std::string body = writer.finish();
    return body + key.sign(body);
}

bool decodeCommitProof(const std::string& encoded, const MigrationKey& key,
                       MigrationCommitProof& proof, std::string& error) {
    std::string body;
    if (!unwrapSignature(encoded, key, body, error)) {
        return false;
    }
    Reader reader(body);
    std::uint64_t value = 0;
    MigrationCommitProof decoded;
    if (!reader.magic(kProofMagic) || !reader.number(2, value) ||
        value != kSlotMigrationVersion) {
        error = "invalid migration commit proof header";
        return false;
    }
    if (!reader.string(decoded.migration_id) || !reader.number(2, value)) {
        error = "truncated migration commit proof";
        return false;
    }
    decoded.slot = static_cast<SlotId>(value);
    if (!reader.number(4, value)) {
        error = "truncated migration commit proof";
        return false;
    }
    decoded.source = static_cast<ShardId>(value);
    if (!reader.number(4, value)) {
        error = "truncated migration commit proof";
        return false;
    }
    decoded.target = static_cast<ShardId>(value);
    if (!reader.number(8, decoded.from_epoch) || !reader.number(8, decoded.to_epoch) ||
        !reader.number(8, decoded.fence_term) ||
        !reader.number(8, decoded.fence_index) ||
        !reader.number(8, decoded.read_index) ||
        !reader.number(8, decoded.fence_sequence) || !reader.number(8, value) ||
        !reader.string(decoded.digest) || !reader.done()) {
        error = "truncated or trailing migration commit proof";
        return false;
    }
    decoded.logical_time_ms = static_cast<UnixMillis>(value);
    if (!validateCommitProof(decoded, error)) {
        return false;
    }
    proof = std::move(decoded);
    return true;
}

bool validateCommitProof(const MigrationCommitProof& proof, std::string& error) {
    if (proof.migration_id.empty() || proof.slot >= kSlotCount ||
        proof.source == kNoShard || proof.target == kNoShard ||
        proof.source == proof.target) {
        error = "migration commit proof identifies no valid migration";
        return false;
    }
    // The upper bound keeps from_epoch + 1 from wrapping back to a value that
    // an old proof could match.
    if (proof.from_epoch == 0 || proof.from_epoch == kMaxOwnershipEpoch ||
        proof.to_epoch != proof.from_epoch + 1) {
        error = "migration commit proof does not advance the ownership epoch";
        return false;
    }
    if (proof.fence_index == 0 || proof.fence_term == 0) {
        error = "migration commit proof has no committed fence entry";
        return false;
    }
    if (proof.read_index < proof.fence_index) {
        error = "migration commit proof read barrier precedes the fence";
        return false;
    }
    if (proof.digest.empty()) {
        error = "migration commit proof carries no state digest";
        return false;
    }
    error.clear();
    return true;
}

std::string encodeActivationProof(const MigrationActivationProof& proof,
                                  const MigrationKey& key) {
    Writer writer;
    writer.magic(kActivationMagic);
    writer.number(kSlotMigrationVersion, 2);
    writer.string(proof.migration_id);
    writer.number(proof.slot, 2);
    writer.number(proof.source, 4);
    writer.number(proof.target, 4);
    writer.number(proof.from_epoch, 8);
    writer.number(proof.to_epoch, 8);
    writer.number(proof.activate_term, 8);
    writer.number(proof.activate_index, 8);
    writer.number(proof.read_index, 8);
    writer.string(proof.digest);
    std::string body = writer.finish();
    return body + key.sign(body);
}

bool decodeActivationProof(const std::string& encoded, const MigrationKey& key,
                           MigrationActivationProof& proof, std::string& error) {
    std::string body;
    if (!unwrapSignature(encoded, key, body, error)) {
        return false;
    }
    Reader reader(body);
    std::uint64_t value = 0;
    MigrationActivationProof decoded;
    if (!reader.magic(kActivationMagic) || !reader.number(2, value) ||
        value != kSlotMigrationVersion) {
        error = "invalid migration activation proof header";
        return false;
    }
    if (!reader.string(decoded.migration_id) || !reader.number(2, value)) {
        error = "truncated migration activation proof";
        return false;
    }
    decoded.slot = static_cast<SlotId>(value);
    if (!reader.number(4, value)) {
        error = "truncated migration activation proof";
        return false;
    }
    decoded.source = static_cast<ShardId>(value);
    if (!reader.number(4, value)) {
        error = "truncated migration activation proof";
        return false;
    }
    decoded.target = static_cast<ShardId>(value);
    if (!reader.number(8, decoded.from_epoch) || !reader.number(8, decoded.to_epoch) ||
        !reader.number(8, decoded.activate_term) ||
        !reader.number(8, decoded.activate_index) ||
        !reader.number(8, decoded.read_index) || !reader.string(decoded.digest) ||
        !reader.done()) {
        error = "truncated or trailing migration activation proof";
        return false;
    }
    if (!validateActivationProof(decoded, error)) {
        return false;
    }
    proof = std::move(decoded);
    return true;
}

bool validateActivationProof(const MigrationActivationProof& proof,
                             std::string& error) {
    if (proof.migration_id.empty() || proof.slot >= kSlotCount ||
        proof.source == kNoShard || proof.target == kNoShard ||
        proof.source == proof.target) {
        error = "migration activation proof identifies no valid migration";
        return false;
    }
    if (proof.from_epoch == 0 || proof.from_epoch == kMaxOwnershipEpoch ||
        proof.to_epoch != proof.from_epoch + 1) {
        error = "migration activation proof does not advance the ownership epoch";
        return false;
    }
    if (proof.activate_index == 0 || proof.activate_term == 0) {
        error = "migration activation proof has no committed activate entry";
        return false;
    }
    if (proof.read_index < proof.activate_index) {
        error = "migration activation proof read barrier precedes the activation";
        return false;
    }
    if (proof.digest.empty()) {
        error = "migration activation proof carries no state digest";
        return false;
    }
    error.clear();
    return true;
}

// --- migration log commands -------------------------------------------------

std::string encodeSlotMigrationCommand(const SlotMigrationCommand& command) {
    Writer writer;
    writer.magic(kCommandMagic);
    writer.number(kSlotMigrationVersion, 2);
    writer.number(static_cast<std::uint8_t>(command.op), 1);
    writer.string(command.migration_id);
    writer.number(command.slot, 2);
    writer.number(command.source, 4);
    writer.number(command.target, 4);
    writer.number(command.from_epoch, 8);
    writer.number(command.to_epoch, 8);
    writer.number(static_cast<std::uint64_t>(command.logical_time_ms), 8);
    writer.number(command.base_complete ? 1U : 0U, 1);
    writer.string(command.commit_proof);
    writer.string(command.activation_proof);
    writer.number(command.records.size(), 4);
    for (const SlotDataRecord& record : command.records) {
        writer.record(record);
    }
    return writer.finish();
}

bool decodeSlotMigrationCommand(const std::string& encoded,
                                SlotMigrationCommand& command, std::string& error) {
    Reader reader(encoded);
    std::uint64_t value = 0;
    SlotMigrationCommand decoded;
    if (encoded.size() > kMaxEncodedBytes || !reader.magic(kCommandMagic) ||
        !reader.number(2, value) || value != kSlotMigrationVersion) {
        error = "invalid slot migration command header";
        return false;
    }
    decoded.version = static_cast<std::uint16_t>(value);
    if (!reader.number(1, value) ||
        value < static_cast<std::uint64_t>(MigrationLogOp::kSourceBegin) ||
        value > static_cast<std::uint64_t>(MigrationLogOp::kForget)) {
        error = "unknown slot migration operation";
        return false;
    }
    decoded.op = static_cast<MigrationLogOp>(value);
    if (!reader.string(decoded.migration_id) || !reader.number(2, value)) {
        error = "truncated slot migration command";
        return false;
    }
    decoded.slot = static_cast<SlotId>(value);
    if (!reader.number(4, value)) {
        error = "truncated slot migration command";
        return false;
    }
    decoded.source = static_cast<ShardId>(value);
    if (!reader.number(4, value)) {
        error = "truncated slot migration command";
        return false;
    }
    decoded.target = static_cast<ShardId>(value);
    std::uint64_t flag = 0;
    std::uint64_t logical_time = 0;
    if (!reader.number(8, decoded.from_epoch) || !reader.number(8, decoded.to_epoch) ||
        !reader.number(8, logical_time) || !reader.number(1, flag) || flag > 1 ||
        !reader.string(decoded.commit_proof) ||
        !reader.string(decoded.activation_proof) || !reader.number(4, value) ||
        !reader.plausibleRecordCount(value)) {
        error = "truncated slot migration command";
        return false;
    }
    decoded.logical_time_ms = static_cast<UnixMillis>(logical_time);
    decoded.base_complete = flag == 1;
    decoded.records.resize(static_cast<std::size_t>(value));
    for (SlotDataRecord& record : decoded.records) {
        if (!reader.record(record)) {
            error = "truncated slot migration record";
            return false;
        }
    }
    if (!reader.done() || decoded.slot >= kSlotCount || decoded.migration_id.empty()) {
        error = "trailing or invalid slot migration command";
        return false;
    }
    command = std::move(decoded);
    error.clear();
    return true;
}

bool isSlotMigrationPayload(const std::string& payload) {
    return payload.size() >= sizeof(kCommandMagic) &&
           std::memcmp(payload.data(), kCommandMagic, sizeof(kCommandMagic)) == 0;
}

// --- state machine ----------------------------------------------------------

MigrationApplyResult SlotMigrationStateMachine::apply(
    const consensus::LogEntry& entry) {
    if (entry.type != consensus::EntryType::kCommand) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "slot migration accepts command entries only");
    }
    SlotMigrationCommand command;
    std::string error;
    if (!decodeSlotMigrationCommand(entry.payload, command, error)) {
        return fail(MigrationApplyStatus::kInvalidCommand, std::move(error));
    }
    return apply(command, entry.index, entry.term);
}

MigrationApplyResult SlotMigrationStateMachine::apply(
    const SlotMigrationCommand& command, consensus::Index log_index,
    consensus::Term log_term) {
    if (command.version != kSlotMigrationVersion || command.migration_id.empty() ||
        command.slot >= kSlotCount || command.source == kNoShard ||
        command.target == kNoShard || command.source == command.target ||
        command.from_epoch == 0 || command.from_epoch == kMaxOwnershipEpoch ||
        command.to_epoch != command.from_epoch + 1) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "malformed slot migration command");
    }
    if (command.source != shard_ && command.target != shard_) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "this shard does not participate in the migration");
    }

    if (command.op == MigrationLogOp::kSourceBegin) {
        return applySourceBegin(command, log_index);
    }
    if (command.op == MigrationLogOp::kTargetBegin) {
        return applyTargetBegin(command);
    }

    const auto it = migrations_.find(command.migration_id);
    if (it == migrations_.end()) {
        return command.op == MigrationLogOp::kForget
                   ? ok(MigrationApplyStatus::kDuplicate)
                   : fail(MigrationApplyStatus::kUnknownMigration,
                          "no local state for this migration");
    }
    LocalMigrationState& state = it->second;
    if (state.slot != command.slot || state.source != command.source ||
        state.target != command.target || state.from_epoch != command.from_epoch ||
        state.to_epoch != command.to_epoch) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "slot migration command contradicts the recorded intent");
    }

    switch (command.op) {
        case MigrationLogOp::kStageBase:
            return applyStageBase(command, state);
        case MigrationLogOp::kStageDelta:
            return applyStageDelta(command, state);
        case MigrationLogOp::kSourceFence:
            return applySourceFence(command, state, log_index, log_term);
        case MigrationLogOp::kTargetActivate:
            return applyTargetActivate(command, state, log_index, log_term);
        case MigrationLogOp::kSourceRelease:
            return applySourceRelease(command, state);
        case MigrationLogOp::kTargetStable:
            return applyTargetStable(state);
        case MigrationLogOp::kAbort:
            return applyAbort(state);
        case MigrationLogOp::kForget: {
            if (state.stage != LocalMigrationStage::kSourceReleased &&
                state.stage != LocalMigrationStage::kTargetStable &&
                state.stage != LocalMigrationStage::kAborted) {
                return fail(MigrationApplyStatus::kIllegalTransition,
                            "migration is still in flight");
            }
            migrations_.erase(it);
            rebuildSourceIndex();
            return ok();
        }
        default:
            return fail(MigrationApplyStatus::kInvalidCommand,
                        "unhandled slot migration operation");
    }
}

MigrationApplyResult SlotMigrationStateMachine::applySourceBegin(
    const SlotMigrationCommand& command, consensus::Index log_index) {
    if (command.source != shard_) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "only the source shard applies SourceBegin");
    }
    const auto existing = migrations_.find(command.migration_id);
    if (existing != migrations_.end()) {
        return existing->second.role == LocalMigrationRole::kSource
                   ? ok(MigrationApplyStatus::kDuplicate)
                   : fail(MigrationApplyStatus::kInvalidCommand,
                          "migration id is already used in another role");
    }
    if (forSlot(command.slot) != nullptr) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "slot already has a migration in flight");
    }
    const LocalSlotOwnership& current = ownership_.get(command.slot);
    if (current.shard != shard_ || current.epoch != command.from_epoch ||
        current.state != LocalSlotState::kStable) {
        return fail(MigrationApplyStatus::kStaleEpoch,
                    "source does not hold the slot at the expected epoch");
    }

    LocalMigrationState state;
    state.id = command.migration_id;
    state.slot = command.slot;
    state.source = command.source;
    state.target = command.target;
    state.from_epoch = command.from_epoch;
    state.to_epoch = command.to_epoch;
    state.role = LocalMigrationRole::kSource;
    state.stage = LocalMigrationStage::kSourceCopying;
    state.base_index = log_index;
    // The base snapshot is materialized here, at one committed index that every
    // replica agrees on. Everything committed after it becomes a delta.
    for (SlotDataRecord& record :
         exportSlotRecords(database_, command.slot, command.logical_time_ms)) {
        const std::string key = record.key;
        state.base.emplace(key, std::move(record));
    }

    MigrationApplyResult result = ok();
    result.records = state.base.size();
    migrations_.emplace(state.id, std::move(state));
    capturing_slots_[command.slot] = command.migration_id;
    return result;
}

MigrationApplyResult SlotMigrationStateMachine::applyTargetBegin(
    const SlotMigrationCommand& command) {
    if (command.target != shard_) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "only the target shard applies TargetBegin");
    }
    const auto existing = migrations_.find(command.migration_id);
    if (existing != migrations_.end()) {
        return existing->second.role == LocalMigrationRole::kTarget
                   ? ok(MigrationApplyStatus::kDuplicate)
                   : fail(MigrationApplyStatus::kInvalidCommand,
                          "migration id is already used in another role");
    }
    if (forSlot(command.slot) != nullptr) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "slot already has a migration in flight");
    }
    const LocalSlotOwnership& current = ownership_.get(command.slot);
    if (current.state != LocalSlotState::kUnowned ||
        current.epoch >= command.to_epoch) {
        return fail(MigrationApplyStatus::kStaleEpoch,
                    "target cannot stage a slot it already owns at this epoch");
    }

    LocalMigrationState state;
    state.id = command.migration_id;
    state.slot = command.slot;
    state.source = command.source;
    state.target = command.target;
    state.from_epoch = command.from_epoch;
    state.to_epoch = command.to_epoch;
    state.role = LocalMigrationRole::kTarget;
    state.stage = LocalMigrationStage::kTargetStaging;
    migrations_.emplace(state.id, std::move(state));
    return ok();
}

MigrationApplyResult SlotMigrationStateMachine::applyStageBase(
    const SlotMigrationCommand& command, LocalMigrationState& state) {
    if (state.role != LocalMigrationRole::kTarget ||
        state.stage != LocalMigrationStage::kTargetStaging) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "base chunks are only accepted by a staging target");
    }
    // Base chunks must all land before the first delta, otherwise a re-delivered
    // chunk could overwrite a newer post-image with snapshot-era content.
    if (state.staged_sequence != 0) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "base chunks cannot follow deltas");
    }
    for (const SlotDataRecord& record : command.records) {
        if (record.sequence != 0 || !record.present ||
            keyToSlot(record.key) != state.slot) {
            return fail(MigrationApplyStatus::kInvalidCommand,
                        "base chunk carries a foreign or malformed record");
        }
    }
    std::size_t changed = 0;
    for (const SlotDataRecord& record : command.records) {
        const auto existing = state.staging.find(record.key);
        if (existing != state.staging.end() &&
            existing->second.value == record.value &&
            existing->second.expire_at_ms == record.expire_at_ms) {
            continue;  // re-delivered chunk
        }
        state.staging[record.key] = record;
        ++changed;
    }
    const bool completed = command.base_complete && !state.base_complete;
    if (command.base_complete) {
        state.base_complete = true;
    }
    MigrationApplyResult result =
        ok(changed == 0 && !completed ? MigrationApplyStatus::kDuplicate
                                      : MigrationApplyStatus::kApplied);
    result.records = changed;
    return result;
}

MigrationApplyResult SlotMigrationStateMachine::applyStageDelta(
    const SlotMigrationCommand& command, LocalMigrationState& state) {
    if (state.role != LocalMigrationRole::kTarget ||
        state.stage != LocalMigrationStage::kTargetStaging) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "deltas are only accepted by a staging target");
    }
    if (!state.base_complete) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "deltas cannot be staged before the base snapshot completes");
    }
    std::uint64_t previous = 0;
    for (const SlotDataRecord& record : command.records) {
        if (record.sequence == 0 || record.sequence <= previous ||
            keyToSlot(record.key) != state.slot) {
            return fail(MigrationApplyStatus::kInvalidCommand,
                        "delta batch is unordered or carries a foreign record");
        }
        previous = record.sequence;
    }
    // A gap would silently drop a key's last post-image, so contiguity is
    // required; anything at or below the staged watermark is a safe replay.
    std::uint64_t expected = state.staged_sequence;
    std::size_t applied = 0;
    for (const SlotDataRecord& record : command.records) {
        if (record.sequence <= state.staged_sequence) {
            continue;
        }
        if (record.sequence != expected + 1) {
            MigrationApplyResult result =
                fail(MigrationApplyStatus::kSequenceGap,
                     "delta batch skips a source sequence number");
            result.sequence = state.staged_sequence;
            return result;
        }
        expected = record.sequence;
        ++applied;
    }
    for (const SlotDataRecord& record : command.records) {
        if (record.sequence <= state.staged_sequence) {
            continue;
        }
        if (record.present) {
            state.staging[record.key] = record;
        } else {
            state.staging.erase(record.key);
        }
    }
    state.staged_sequence = expected;
    MigrationApplyResult result = ok(
        applied == 0 ? MigrationApplyStatus::kDuplicate : MigrationApplyStatus::kApplied);
    result.sequence = state.staged_sequence;
    result.records = applied;
    return result;
}

MigrationApplyResult SlotMigrationStateMachine::applySourceFence(
    const SlotMigrationCommand& command, LocalMigrationState& state,
    consensus::Index log_index, consensus::Term log_term) {
    if (state.role != LocalMigrationRole::kSource) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "only the source shard can fence a slot");
    }
    if (state.stage == LocalMigrationStage::kSourceFenced ||
        state.stage == LocalMigrationStage::kSourceReleased) {
        // Never recompute: the proof already handed to the control plane must
        // keep matching this record after a re-proposal.
        MigrationApplyResult result = ok(MigrationApplyStatus::kDuplicate);
        result.sequence = state.fence_sequence;
        return result;
    }
    if (state.stage != LocalMigrationStage::kSourceCopying) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "migration is no longer fenceable");
    }

    LocalSlotOwnership fenced = ownership_.get(state.slot);
    if (fenced.shard != shard_ || fenced.epoch != state.from_epoch ||
        fenced.state != LocalSlotState::kStable) {
        return fail(MigrationApplyStatus::kStaleEpoch,
                    "source ownership changed before the fence");
    }
    fenced.state = LocalSlotState::kSourceFenced;
    fenced.migration_id = state.id;
    std::string error;
    if (!ownership_.update(state.slot, state.from_epoch, fenced, error)) {
        return fail(MigrationApplyStatus::kStaleEpoch, std::move(error));
    }

    state.stage = LocalMigrationStage::kSourceFenced;
    state.fence_index = log_index;
    state.fence_term = log_term;
    state.fence_sequence = state.next_sequence - 1;
    state.fence_logical_time_ms = command.logical_time_ms;
    state.fence_digest = slotDigest(
        exportSlotRecords(database_, state.slot, command.logical_time_ms),
        command.logical_time_ms);
    capturing_slots_.erase(state.slot);

    MigrationApplyResult result = ok();
    result.sequence = state.fence_sequence;
    return result;
}

MigrationApplyResult SlotMigrationStateMachine::applyTargetActivate(
    const SlotMigrationCommand& command, LocalMigrationState& state,
    consensus::Index log_index, consensus::Term log_term) {
    if (state.role != LocalMigrationRole::kTarget) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "only the target shard can activate a slot");
    }
    MigrationCommitProof proof;
    std::string error;
    if (!decodeCommitProof(command.commit_proof, key_, proof, error)) {
        return fail(MigrationApplyStatus::kProofRejected, std::move(error));
    }
    if (proof.migration_id != state.id || proof.slot != state.slot ||
        proof.source != state.source || proof.target != state.target ||
        proof.from_epoch != state.from_epoch || proof.to_epoch != state.to_epoch) {
        return fail(MigrationApplyStatus::kProofRejected,
                    "commit proof does not match the recorded intent");
    }
    if (state.stage == LocalMigrationStage::kTargetActive ||
        state.stage == LocalMigrationStage::kTargetStable) {
        return proof.digest == state.fence_digest
                   ? ok(MigrationApplyStatus::kDuplicate)
                   : fail(MigrationApplyStatus::kProofRejected,
                          "a different proof already activated this slot");
    }
    if (state.stage != LocalMigrationStage::kTargetStaging) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "migration is not stageable");
    }
    if (!state.base_complete) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "base snapshot is incomplete");
    }
    if (state.staged_sequence < proof.fence_sequence) {
        MigrationApplyResult result =
            fail(MigrationApplyStatus::kSequenceGap,
                 "target has not processed every change up to the fence");
        result.sequence = state.staged_sequence;
        return result;
    }

    // Checked before anything is written: reading the current epoch back into
    // the CAS below would make the compare vacuous, and a second activation
    // would then drop the keyspace this slot has been serving since the first.
    const LocalSlotOwnership& current = ownership_.get(state.slot);
    if (current.state != LocalSlotState::kUnowned || current.epoch >= state.to_epoch) {
        return fail(MigrationApplyStatus::kStaleEpoch,
                    "the target no longer holds the slot in a stageable state");
    }

    std::vector<SlotDataRecord> staged;
    staged.reserve(state.staging.size());
    for (const auto& [key, record] : state.staging) {
        (void)key;
        staged.push_back(record);
    }
    if (slotDigest(staged, proof.logical_time_ms) != proof.digest) {
        return fail(MigrationApplyStatus::kDigestMismatch,
                    "staged slot content does not match the fenced source");
    }

    // Decode everything before touching the database so a malformed record
    // cannot leave the slot half-populated.
    std::vector<std::pair<const SlotDataRecord*, std::shared_ptr<RedisObject>>> ready;
    ready.reserve(staged.size());
    for (const SlotDataRecord& record : staged) {
        if (!record.present || expiredAt(record, proof.logical_time_ms)) {
            continue;
        }
        std::shared_ptr<RedisObject> object = decodeValue(record.value, error);
        if (!object) {
            return fail(MigrationApplyStatus::kInvalidCommand, std::move(error));
        }
        ready.emplace_back(&record, std::move(object));
    }

    const SlotId slot = state.slot;
    database_.dropKeys(
        [slot](const std::string& key) { return keyToSlot(key) == slot; });
    for (auto& [record, object] : ready) {
        database_.importKey(record->key, std::move(object), record->expire_at_ms);
    }

    LocalSlotOwnership activated;
    activated.shard = shard_;
    activated.epoch = state.to_epoch;
    activated.state = LocalSlotState::kTargetActiveAsk;
    activated.migration_id = state.id;
    const std::uint64_t expected_epoch = current.epoch;
    if (!ownership_.update(state.slot, expected_epoch, activated, error)) {
        return fail(MigrationApplyStatus::kStaleEpoch, std::move(error));
    }

    state.stage = LocalMigrationStage::kTargetActive;
    state.fence_index = proof.fence_index;
    state.fence_term = proof.fence_term;
    state.fence_sequence = proof.fence_sequence;
    state.fence_logical_time_ms = proof.logical_time_ms;
    state.fence_digest = proof.digest;
    state.activate_index = log_index;
    state.activate_term = log_term;
    state.staging.clear();

    MigrationApplyResult result = ok();
    result.sequence = state.staged_sequence;
    result.records = ready.size();
    return result;
}

MigrationApplyResult SlotMigrationStateMachine::applySourceRelease(
    const SlotMigrationCommand& command, LocalMigrationState& state) {
    if (state.role != LocalMigrationRole::kSource) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "only the source shard releases a slot");
    }
    if (state.stage == LocalMigrationStage::kSourceReleased) {
        return ok(MigrationApplyStatus::kDuplicate);
    }
    if (state.stage != LocalMigrationStage::kSourceFenced) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "only a fenced source may release its slot");
    }

    // Deleting the slot here destroys what may still be its only copy, so the
    // source demands committed evidence that the target holds the same state.
    MigrationActivationProof proof;
    std::string proof_error;
    if (!decodeActivationProof(command.activation_proof, key_, proof, proof_error)) {
        return fail(MigrationApplyStatus::kProofRejected, std::move(proof_error));
    }
    if (proof.migration_id != state.id || proof.slot != state.slot ||
        proof.source != state.source || proof.target != state.target ||
        proof.from_epoch != state.from_epoch || proof.to_epoch != state.to_epoch) {
        return fail(MigrationApplyStatus::kProofRejected,
                    "activation proof does not match the recorded intent");
    }
    if (proof.digest != state.fence_digest) {
        return fail(MigrationApplyStatus::kDigestMismatch,
                    "the target activated a state the source never fenced");
    }

    // The record survives the release so that re-acquiring this slot later
    // still has to present a strictly newer epoch. It is stamped with to_epoch,
    // not from_epoch: the activation proof just established that ownership has
    // advanced, and leaving the old number here would let a migration back into
    // this shard reuse an epoch the cluster has already moved past.
    LocalSlotOwnership released;
    released.shard = kNoShard;
    released.epoch = state.to_epoch;
    released.state = LocalSlotState::kUnowned;
    std::string error;
    if (!ownership_.update(state.slot, state.from_epoch, released, error)) {
        return fail(MigrationApplyStatus::kStaleEpoch, std::move(error));
    }

    const SlotId slot = state.slot;
    const std::size_t dropped = database_.dropKeys(
        [slot](const std::string& key) { return keyToSlot(key) == slot; });
    state.stage = LocalMigrationStage::kSourceReleased;
    state.base.clear();
    state.deltas.clear();

    MigrationApplyResult result = ok();
    result.records = dropped;
    return result;
}

MigrationApplyResult SlotMigrationStateMachine::applyTargetStable(
    LocalMigrationState& state) {
    if (state.role != LocalMigrationRole::kTarget) {
        return fail(MigrationApplyStatus::kInvalidCommand,
                    "only the target shard leaves ASK-only mode");
    }
    if (state.stage == LocalMigrationStage::kTargetStable) {
        return ok(MigrationApplyStatus::kDuplicate);
    }
    if (state.stage != LocalMigrationStage::kTargetActive) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "target is not active yet");
    }
    LocalSlotOwnership stable = ownership_.get(state.slot);
    if (stable.shard != shard_ || stable.epoch != state.to_epoch ||
        stable.state != LocalSlotState::kTargetActiveAsk) {
        return fail(MigrationApplyStatus::kStaleEpoch,
                    "target ownership changed after activation");
    }
    stable.state = LocalSlotState::kStable;
    stable.migration_id.clear();
    std::string error;
    if (!ownership_.update(state.slot, state.to_epoch, stable, error)) {
        return fail(MigrationApplyStatus::kStaleEpoch, std::move(error));
    }
    state.stage = LocalMigrationStage::kTargetStable;
    return ok();
}

MigrationApplyResult SlotMigrationStateMachine::applyAbort(
    LocalMigrationState& state) {
    if (state.stage == LocalMigrationStage::kAborted) {
        return ok(MigrationApplyStatus::kDuplicate);
    }
    // This is the one-way door. After the fence the source has already promised
    // the control plane it will never serve the old epoch again, so the only
    // way out is forward.
    if (state.stage != LocalMigrationStage::kSourceCopying &&
        state.stage != LocalMigrationStage::kTargetStaging) {
        return fail(MigrationApplyStatus::kIllegalTransition,
                    "a fenced or activated migration can only move forward");
    }
    state.stage = LocalMigrationStage::kAborted;
    state.base.clear();
    state.deltas.clear();
    state.staging.clear();
    state.base_complete = false;
    capturing_slots_.erase(state.slot);
    return ok();
}

void SlotMigrationStateMachine::captureWrite(SlotId slot,
                                             const std::vector<std::string>& keys,
                                             UnixMillis logical_time_ms) {
    const auto slot_entry = capturing_slots_.find(slot);
    if (slot_entry == capturing_slots_.end() || keys.empty()) {
        return;
    }
    const auto migration = migrations_.find(slot_entry->second);
    if (migration == migrations_.end() ||
        migration->second.stage != LocalMigrationStage::kSourceCopying) {
        return;
    }
    LocalMigrationState& state = migration->second;
    // Sorted and de-duplicated so the sequence numbers a replica assigns do not
    // depend on argument order or on repeated keys within one command.
    const std::set<std::string> unique(keys.begin(), keys.end());
    for (const std::string& key : unique) {
        if (keyToSlot(key) != slot) {
            continue;
        }
        SlotDataRecord record;
        record.key = key;
        StoredEntry entry;
        if (database_.exportKey(key, entry) && entry.object &&
            !(entry.expire_at_ms != 0 && entry.expire_at_ms <= logical_time_ms)) {
            record.present = true;
            record.value = encodeValue(*entry.object);
            record.expire_at_ms = entry.expire_at_ms;
        } else if (state.base.count(key) == 0 &&
                   state.journaled_keys.count(key) == 0) {
            // Deleting something the target has never been told about. The
            // tombstone would apply to nothing, so it is not worth a sequence
            // number. Every replica reaches this conclusion from the same
            // committed base and journal, so the numbering stays identical.
            continue;
        }
        record.sequence = state.next_sequence++;
        state.journaled_keys.insert(key);
        state.deltas.emplace(record.sequence, std::move(record));
    }
}

const LocalMigrationState* SlotMigrationStateMachine::find(
    const std::string& migration_id) const {
    const auto it = migrations_.find(migration_id);
    return it == migrations_.end() ? nullptr : &it->second;
}

const LocalMigrationState* SlotMigrationStateMachine::forSlot(SlotId slot) const {
    for (const auto& [id, state] : migrations_) {
        (void)id;
        if (state.slot != slot) continue;
        if (state.stage == LocalMigrationStage::kAborted ||
            state.stage == LocalMigrationStage::kSourceReleased ||
            state.stage == LocalMigrationStage::kTargetStable) {
            continue;
        }
        return &state;
    }
    return nullptr;
}

std::vector<SlotDataRecord> SlotMigrationStateMachine::baseChunkAfter(
    const std::string& migration_id, const std::string& after_key,
    std::size_t max_records, bool& complete) const {
    complete = true;
    std::vector<SlotDataRecord> chunk;
    const LocalMigrationState* state = find(migration_id);
    if (state == nullptr || state->role != LocalMigrationRole::kSource) {
        return chunk;
    }
    auto it = after_key.empty() ? state->base.begin()
                                : state->base.upper_bound(after_key);
    for (; it != state->base.end() && chunk.size() < max_records; ++it) {
        chunk.push_back(it->second);
    }
    complete = it == state->base.end();
    return chunk;
}

std::vector<SlotDataRecord> SlotMigrationStateMachine::deltasAfter(
    const std::string& migration_id, std::uint64_t after_sequence,
    std::size_t max_records) const {
    std::vector<SlotDataRecord> batch;
    const LocalMigrationState* state = find(migration_id);
    if (state == nullptr || state->role != LocalMigrationRole::kSource) {
        return batch;
    }
    for (auto it = state->deltas.upper_bound(after_sequence);
         it != state->deltas.end() && batch.size() < max_records; ++it) {
        batch.push_back(it->second);
    }
    return batch;
}

bool SlotMigrationStateMachine::buildCommitProof(const std::string& migration_id,
                                                 consensus::Index read_index,
                                                 MigrationCommitProof& proof,
                                                 std::string& error) const {
    const LocalMigrationState* state = find(migration_id);
    if (state == nullptr || state->role != LocalMigrationRole::kSource) {
        error = "no local source state for this migration";
        return false;
    }
    if (state->stage != LocalMigrationStage::kSourceFenced &&
        state->stage != LocalMigrationStage::kSourceReleased) {
        error = "the source is not fenced yet";
        return false;
    }
    if (read_index < state->fence_index) {
        error = "read barrier does not cover the fence entry";
        return false;
    }

    MigrationCommitProof built;
    built.migration_id = state->id;
    built.slot = state->slot;
    built.source = state->source;
    built.target = state->target;
    built.from_epoch = state->from_epoch;
    built.to_epoch = state->to_epoch;
    built.fence_term = state->fence_term;
    built.fence_index = state->fence_index;
    built.read_index = read_index;
    built.fence_sequence = state->fence_sequence;
    built.logical_time_ms = state->fence_logical_time_ms;
    built.digest = state->fence_digest;
    if (!validateCommitProof(built, error)) {
        return false;
    }
    proof = std::move(built);
    return true;
}

bool SlotMigrationStateMachine::buildActivationProof(
    const std::string& migration_id, consensus::Index read_index,
    MigrationActivationProof& proof, std::string& error) const {
    const LocalMigrationState* state = find(migration_id);
    if (state == nullptr || state->role != LocalMigrationRole::kTarget) {
        error = "no local target state for this migration";
        return false;
    }
    if (state->stage != LocalMigrationStage::kTargetActive &&
        state->stage != LocalMigrationStage::kTargetStable) {
        error = "the target has not activated yet";
        return false;
    }
    if (read_index < state->activate_index) {
        error = "read barrier does not cover the activate entry";
        return false;
    }

    MigrationActivationProof built;
    built.migration_id = state->id;
    built.slot = state->slot;
    built.source = state->source;
    built.target = state->target;
    built.from_epoch = state->from_epoch;
    built.to_epoch = state->to_epoch;
    built.activate_term = state->activate_term;
    built.activate_index = state->activate_index;
    built.read_index = read_index;
    built.digest = state->fence_digest;
    if (!validateActivationProof(built, error)) {
        return false;
    }
    proof = std::move(built);
    return true;
}

void SlotMigrationStateMachine::rebuildSourceIndex() {
    capturing_slots_.clear();
    for (auto& [id, state] : migrations_) {
        state.journaled_keys.clear();
        for (const auto& [sequence, record] : state.deltas) {
            (void)sequence;
            state.journaled_keys.insert(record.key);
        }
        if (state.role == LocalMigrationRole::kSource &&
            state.stage == LocalMigrationStage::kSourceCopying) {
            capturing_slots_[state.slot] = id;
        }
    }
}

std::string SlotMigrationStateMachine::snapshotBytes() const {
    Writer writer;
    writer.magic(kSnapshotMagic);
    writer.number(kSlotMigrationVersion, 2);
    writer.number(shard_, 4);
    writer.number(migrations_.size(), 4);
    for (const auto& [id, state] : migrations_) {
        writer.string(id);
        writer.number(state.slot, 2);
        writer.number(state.source, 4);
        writer.number(state.target, 4);
        writer.number(state.from_epoch, 8);
        writer.number(state.to_epoch, 8);
        writer.number(static_cast<std::uint8_t>(state.role), 1);
        writer.number(static_cast<std::uint8_t>(state.stage), 1);
        writer.number(state.base_index, 8);
        writer.number(state.fence_index, 8);
        writer.number(state.fence_term, 8);
        writer.number(state.next_sequence, 8);
        writer.number(state.fence_sequence, 8);
        writer.number(static_cast<std::uint64_t>(state.fence_logical_time_ms), 8);
        writer.string(state.fence_digest);
        writer.number(state.base_complete ? 1U : 0U, 1);
        writer.number(state.staged_sequence, 8);
        writer.number(state.activate_index, 8);
        writer.number(state.activate_term, 8);
        writer.number(state.base.size(), 4);
        for (const auto& [key, record] : state.base) {
            (void)key;
            writer.record(record);
        }
        writer.number(state.deltas.size(), 4);
        for (const auto& [sequence, record] : state.deltas) {
            (void)sequence;
            writer.record(record);
        }
        writer.number(state.staging.size(), 4);
        for (const auto& [key, record] : state.staging) {
            (void)key;
            writer.record(record);
        }
    }
    return writer.finish();
}

bool SlotMigrationStateMachine::installSnapshot(const std::string& bytes,
                                                std::string& error) {
    Reader reader(bytes);
    std::uint64_t value = 0;
    if (bytes.size() > kMaxEncodedBytes || !reader.magic(kSnapshotMagic) ||
        !reader.number(2, value) || value != kSlotMigrationVersion) {
        error = "invalid slot migration snapshot header";
        return false;
    }
    if (!reader.number(4, value) || static_cast<ShardId>(value) != shard_) {
        error = "slot migration snapshot belongs to another shard";
        return false;
    }
    std::uint64_t count = 0;
    if (!reader.number(4, count) || count > kMaxItems) {
        error = "invalid slot migration snapshot size";
        return false;
    }

    const std::uint64_t migration_count = count;
    std::map<std::string, LocalMigrationState> restored;
    for (std::uint64_t index = 0; index < migration_count; ++index) {
        LocalMigrationState state;
        if (!reader.string(state.id) || !reader.number(2, value)) {
            error = "truncated slot migration snapshot";
            return false;
        }
        state.slot = static_cast<SlotId>(value);
        if (!reader.number(4, value)) {
            error = "truncated slot migration snapshot";
            return false;
        }
        state.source = static_cast<ShardId>(value);
        if (!reader.number(4, value)) {
            error = "truncated slot migration snapshot";
            return false;
        }
        state.target = static_cast<ShardId>(value);
        std::uint64_t role = 0;
        std::uint64_t stage = 0;
        std::uint64_t logical_time = 0;
        std::uint64_t base_complete = 0;
        // role 0 is kNone: a record in that state matches no apply path and
        // would sit in the map forever, blocking the slot via forSlot().
        if (!reader.number(8, state.from_epoch) || !reader.number(8, state.to_epoch) ||
            !reader.number(1, role) || role < 1 || role > 2 ||
            !reader.number(1, stage) ||
            stage < 1 || stage > 7 || !reader.number(8, state.base_index) ||
            !reader.number(8, state.fence_index) ||
            !reader.number(8, state.fence_term) ||
            !reader.number(8, state.next_sequence) ||
            !reader.number(8, state.fence_sequence) ||
            !reader.number(8, logical_time) || !reader.string(state.fence_digest) ||
            !reader.number(1, base_complete) || base_complete > 1 ||
            !reader.number(8, state.staged_sequence) ||
            !reader.number(8, state.activate_index) ||
            !reader.number(8, state.activate_term)) {
            error = "truncated slot migration snapshot";
            return false;
        }
        state.role = static_cast<LocalMigrationRole>(role);
        state.stage = static_cast<LocalMigrationStage>(stage);
        state.fence_logical_time_ms = static_cast<UnixMillis>(logical_time);
        state.base_complete = base_complete == 1;

        for (int section = 0; section < 3; ++section) {
            std::uint64_t items = 0;
            if (!reader.number(4, items) || !reader.plausibleRecordCount(items)) {
                error = "truncated slot migration snapshot section";
                return false;
            }
            for (std::uint64_t item = 0; item < items; ++item) {
                SlotDataRecord record;
                if (!reader.record(record)) {
                    error = "truncated slot migration record";
                    return false;
                }
                if (section == 0) {
                    state.base[record.key] = std::move(record);
                } else if (section == 1) {
                    const std::uint64_t sequence = record.sequence;
                    state.deltas[sequence] = std::move(record);
                } else {
                    state.staging[record.key] = std::move(record);
                }
            }
        }
        if (state.id.empty() || state.slot >= kSlotCount ||
            state.source == kNoShard || state.target == kNoShard ||
            state.source == state.target || state.from_epoch == 0 ||
            state.from_epoch == kMaxOwnershipEpoch ||
            state.to_epoch != state.from_epoch + 1) {
            error = "invalid slot migration snapshot entry";
            return false;
        }
        // The snapshot has to describe this shard's own role, and the role has
        // to be the side of the migration this shard is actually on.
        const bool is_source = state.role == LocalMigrationRole::kSource;
        if ((is_source ? state.source : state.target) != shard_) {
            error = "slot migration snapshot assigns this shard the wrong role";
            return false;
        }
        if (!restored.emplace(state.id, std::move(state)).second) {
            error = "duplicate migration id in slot migration snapshot";
            return false;
        }
    }
    if (!reader.done()) {
        error = "trailing slot migration snapshot data";
        return false;
    }
    migrations_ = std::move(restored);
    rebuildSourceIndex();
    error.clear();
    return true;
}

} // namespace cluster
