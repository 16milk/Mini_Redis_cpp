#include "mini_redis/cluster/MetadataCommand.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace cluster {
namespace {

constexpr char kMagic[] = {'M', 'R', 'M', 'D'};
constexpr std::size_t kMaxEncodedBytes = 64U << 20;
constexpr std::size_t kMaxItems = 1U << 20;

void appendUnsigned(std::string& out, std::uint64_t value, unsigned width) {
    for (unsigned shift = width * 8; shift != 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> (shift - 8)) & 0xffU));
    }
}

bool readUnsigned(std::string_view input, std::size_t& offset, unsigned width,
                  std::uint64_t& value) {
    if (width > input.size() - offset) {
        return false;
    }
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

void appendEndpoint(std::string& out, const Endpoint& endpoint) {
    appendString(out, endpoint.host);
    appendUnsigned(out, endpoint.port, 2);
}

bool readEndpoint(std::string_view input, std::size_t& offset, Endpoint& endpoint) {
    std::uint64_t port = 0;
    return readString(input, offset, endpoint.host) &&
           readUnsigned(input, offset, 2, port) &&
           ((endpoint.port = static_cast<std::uint16_t>(port)), true);
}

void appendNodes(std::string& out, const std::vector<NodeId>& nodes) {
    appendUnsigned(out, nodes.size(), 4);
    for (const NodeId& node : nodes) {
        appendString(out, node);
    }
}

bool readNodes(std::string_view input, std::size_t& offset, std::vector<NodeId>& nodes) {
    std::uint64_t count = 0;
    if (!readUnsigned(input, offset, 4, count) || count > kMaxItems) {
        return false;
    }
    nodes.clear();
    nodes.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        NodeId node;
        if (!readString(input, offset, node)) {
            return false;
        }
        nodes.push_back(std::move(node));
    }
    return true;
}

bool validOperation(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(MetadataOperation::kInitializeCluster) &&
           value <= static_cast<std::uint64_t>(MetadataOperation::kFinishMigration);
}

bool validNodeStatus(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(MetadataNodeStatus::kJoining) &&
           value <= static_cast<std::uint64_t>(MetadataNodeStatus::kRemoved);
}

bool validMigrationPhase(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(MigrationPhase::kPreparing) &&
           value <= static_cast<std::uint64_t>(MigrationPhase::kAborted);
}

} // namespace

std::string encodeMetadataCommand(const MetadataCommand& command) {
    std::string out(kMagic, sizeof(kMagic));
    appendUnsigned(out, command.version, 2);
    appendUnsigned(out, static_cast<std::uint8_t>(command.operation), 1);
    appendString(out, command.request_id);
    appendUnsigned(out, command.expected_revision, 8);
    appendString(out, command.cluster_id);

    appendString(out, command.node.id);
    appendEndpoint(out, command.node.client);
    appendEndpoint(out, command.node.internal);
    appendEndpoint(out, command.node.admin);
    appendString(out, command.node.failure_domain);
    appendUnsigned(out, static_cast<std::uint8_t>(command.node.status), 1);
    appendString(out, command.node_id);

    appendUnsigned(out, command.group.id, 4);
    appendNodes(out, command.group.voters);
    appendNodes(out, command.group.learners);
    appendNodes(out, command.group.desired_placement);
    appendUnsigned(out, command.group.config_index, 8);
    appendUnsigned(out, command.group_id, 4);

    appendUnsigned(out, command.slot_start, 2);
    appendUnsigned(out, command.slot_end, 2);
    appendUnsigned(out, command.owner_group, 4);
    appendUnsigned(out, command.expected_ownership_epoch, 8);
    appendUnsigned(out, command.new_ownership_epoch, 8);

    appendString(out, command.migration_id);
    appendUnsigned(out, command.source_group, 4);
    appendUnsigned(out, command.target_group, 4);
    appendUnsigned(out, static_cast<std::uint8_t>(command.migration_phase), 1);
    appendString(out, command.progress_proof);
    return out;
}

bool decodeMetadataCommand(const std::string& encoded, MetadataCommand& command,
                           std::string& error) {
    if (encoded.size() > kMaxEncodedBytes ||
        encoded.size() < sizeof(kMagic) ||
        !std::equal(std::begin(kMagic), std::end(kMagic), encoded.begin())) {
        error = "invalid metadata command header";
        return false;
    }

    MetadataCommand decoded;
    std::size_t offset = sizeof(kMagic);
    std::uint64_t value = 0;
    if (!readUnsigned(encoded, offset, 2, value)) {
        error = "truncated metadata command version";
        return false;
    }
    decoded.version = static_cast<std::uint16_t>(value);
    if (!readUnsigned(encoded, offset, 1, value) || !validOperation(value)) {
        error = "invalid metadata operation";
        return false;
    }
    decoded.operation = static_cast<MetadataOperation>(value);
    if (!readString(encoded, offset, decoded.request_id) ||
        !readUnsigned(encoded, offset, 8, decoded.expected_revision) ||
        !readString(encoded, offset, decoded.cluster_id) ||
        !readString(encoded, offset, decoded.node.id) ||
        !readEndpoint(encoded, offset, decoded.node.client) ||
        !readEndpoint(encoded, offset, decoded.node.internal) ||
        !readEndpoint(encoded, offset, decoded.node.admin) ||
        !readString(encoded, offset, decoded.node.failure_domain) ||
        !readUnsigned(encoded, offset, 1, value) || !validNodeStatus(value)) {
        error = "truncated or invalid metadata node";
        return false;
    }
    decoded.node.status = static_cast<MetadataNodeStatus>(value);
    if (!readString(encoded, offset, decoded.node_id) ||
        !readUnsigned(encoded, offset, 4, value)) {
        error = "truncated metadata group";
        return false;
    }
    decoded.group.id = static_cast<ShardId>(value);
    if (!readNodes(encoded, offset, decoded.group.voters) ||
        !readNodes(encoded, offset, decoded.group.learners) ||
        !readNodes(encoded, offset, decoded.group.desired_placement) ||
        !readUnsigned(encoded, offset, 8, decoded.group.config_index) ||
        !readUnsigned(encoded, offset, 4, value)) {
        error = "truncated metadata group members";
        return false;
    }
    decoded.group_id = static_cast<ShardId>(value);

    if (!readUnsigned(encoded, offset, 2, value)) return false;
    decoded.slot_start = static_cast<SlotId>(value);
    if (!readUnsigned(encoded, offset, 2, value)) return false;
    decoded.slot_end = static_cast<SlotId>(value);
    if (!readUnsigned(encoded, offset, 4, value)) return false;
    decoded.owner_group = static_cast<ShardId>(value);
    if (!readUnsigned(encoded, offset, 8, decoded.expected_ownership_epoch) ||
        !readUnsigned(encoded, offset, 8, decoded.new_ownership_epoch) ||
        !readString(encoded, offset, decoded.migration_id) ||
        !readUnsigned(encoded, offset, 4, value)) {
        error = "truncated metadata slot or migration";
        return false;
    }
    decoded.source_group = static_cast<ShardId>(value);
    if (!readUnsigned(encoded, offset, 4, value)) return false;
    decoded.target_group = static_cast<ShardId>(value);
    if (!readUnsigned(encoded, offset, 1, value) || !validMigrationPhase(value) ||
        !readString(encoded, offset, decoded.progress_proof) ||
        offset != encoded.size()) {
        error = "trailing or invalid metadata migration";
        return false;
    }
    decoded.migration_phase = static_cast<MigrationPhase>(value);
    if (decoded.version != kMetadataCommandVersion || decoded.request_id.empty()) {
        error = "unsupported metadata command version or empty request id";
        return false;
    }
    command = std::move(decoded);
    error.clear();
    return true;
}

} // namespace cluster
