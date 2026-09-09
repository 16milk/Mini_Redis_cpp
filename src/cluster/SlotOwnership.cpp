#include "mini_redis/cluster/SlotOwnership.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace cluster {
namespace {

constexpr char kMagic[] = {'M', 'R', 'O', 'S'};
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxSnapshotBytes = 64U << 20;

void appendUnsigned(std::string& out, std::uint64_t value, unsigned width) {
    for (unsigned shift = width * 8; shift != 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> (shift - 8)) & 0xffU));
    }
}

bool readUnsigned(std::string_view input, std::size_t& offset, unsigned width,
                  std::uint64_t& value) {
    if (width > input.size() - offset) return false;
    value = 0;
    for (unsigned index = 0; index < width; ++index) {
        value = (value << 8) |
                static_cast<unsigned char>(input[offset++]);
    }
    return true;
}

void appendString(std::string& out, const std::string& value) {
    appendUnsigned(out, value.size(), 4);
    out.append(value);
}

bool readString(std::string_view input, std::size_t& offset, std::string& value) {
    std::uint64_t size = 0;
    if (!readUnsigned(input, offset, 4, size) || size > input.size() - offset) {
        return false;
    }
    value.assign(input.data() + offset, static_cast<std::size_t>(size));
    offset += static_cast<std::size_t>(size);
    return true;
}

bool validState(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(LocalSlotState::kTargetActiveAsk);
}

} // namespace

bool SlotOwnershipTable::update(SlotId slot, std::uint64_t expected_epoch,
                                LocalSlotOwnership replacement,
                                std::string& error) {
    if (slot >= kSlotCount) {
        error = "slot ownership update is out of range";
        return false;
    }
    const LocalSlotOwnership& current = slots_[slot];
    if (current.epoch != expected_epoch) {
        error = "local slot ownership epoch CAS mismatch";
        return false;
    }
    if (replacement.shard == kNoShard) {
        if (replacement.state != LocalSlotState::kUnowned) {
            error = "unowned slot has an active state";
            return false;
        }
    } else if (replacement.epoch == 0 ||
               replacement.state == LocalSlotState::kUnowned) {
        error = "owned slot requires a non-zero epoch and active state";
        return false;
    }
    if (replacement.epoch < current.epoch) {
        error = "local slot ownership epoch cannot go backwards";
        return false;
    }
    if (current.shard != kNoShard && replacement.shard != kNoShard &&
        replacement.shard != current.shard &&
        replacement.epoch <= current.epoch) {
        error = "changing the local owner requires a newer epoch";
        return false;
    }
    if (current.state == LocalSlotState::kSourceFenced &&
        replacement.shard == current.shard &&
        replacement.epoch == current.epoch &&
        replacement.state == LocalSlotState::kStable) {
        error = "a fenced source cannot reopen at the same epoch";
        return false;
    }
    slots_[slot] = std::move(replacement);
    error.clear();
    return true;
}

bool SlotOwnershipTable::canAcceptWrite(SlotId slot, ShardId shard,
                                        std::uint64_t epoch, bool asking,
                                        std::string* error) const {
    if (slot >= kSlotCount) {
        if (error != nullptr) *error = "slot is out of range";
        return false;
    }
    const LocalSlotOwnership& ownership = slots_[slot];
    std::string failure;
    if (ownership.shard != shard || ownership.epoch != epoch) {
        failure = "local committed ownership does not match topology";
    } else if (ownership.state == LocalSlotState::kSourceFenced) {
        failure = "slot source is fenced";
    } else if (ownership.state == LocalSlotState::kTargetActiveAsk && !asking) {
        failure = "migration target requires ASKING";
    } else if (ownership.state != LocalSlotState::kStable &&
               ownership.state != LocalSlotState::kTargetActiveAsk) {
        failure = "slot is not writable";
    }
    if (error != nullptr) *error = failure;
    return failure.empty();
}

std::string SlotOwnershipTable::snapshotBytes() const {
    std::string out(kMagic, sizeof(kMagic));
    appendUnsigned(out, kVersion, 2);
    for (const LocalSlotOwnership& slot : slots_) {
        appendUnsigned(out, slot.shard, 4);
        appendUnsigned(out, slot.epoch, 8);
        appendUnsigned(out, static_cast<std::uint8_t>(slot.state), 1);
        appendString(out, slot.migration_id);
    }
    return out;
}

bool SlotOwnershipTable::installSnapshot(const std::string& bytes,
                                         std::string& error) {
    if (bytes.size() > kMaxSnapshotBytes ||
        bytes.size() < sizeof(kMagic) + 2 ||
        !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
        error = "invalid slot ownership snapshot header";
        return false;
    }
    std::size_t offset = sizeof(kMagic);
    std::uint64_t value = 0;
    if (!readUnsigned(bytes, offset, 2, value) || value != kVersion) {
        error = "unsupported slot ownership snapshot version";
        return false;
    }
    std::array<LocalSlotOwnership, kSlotCount> restored{};
    for (LocalSlotOwnership& slot : restored) {
        if (!readUnsigned(bytes, offset, 4, value)) return false;
        slot.shard = static_cast<ShardId>(value);
        if (!readUnsigned(bytes, offset, 8, slot.epoch) ||
            !readUnsigned(bytes, offset, 1, value) || !validState(value) ||
            !readString(bytes, offset, slot.migration_id)) {
            error = "truncated slot ownership snapshot";
            return false;
        }
        slot.state = static_cast<LocalSlotState>(value);
        if ((slot.shard == kNoShard) !=
            (slot.state == LocalSlotState::kUnowned)) {
            error = "inconsistent slot ownership snapshot";
            return false;
        }
    }
    if (offset != bytes.size()) {
        error = "trailing slot ownership snapshot data";
        return false;
    }
    slots_ = std::move(restored);
    error.clear();
    return true;
}

} // namespace cluster
