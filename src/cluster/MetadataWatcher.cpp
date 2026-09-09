#include "mini_redis/cluster/MetadataWatcher.hpp"

#include <algorithm>
#include <stdexcept>

namespace cluster {

TopologyPtr buildTopologySnapshot(const ClusterMetadata& metadata,
                                  const TopologyPtr& previous) {
    if (!metadata.initialized()) {
        throw std::invalid_argument("cannot publish uninitialized cluster metadata");
    }

    TopologyBuilder builder;
    builder.setTopologyRevision(metadata.revision);
    for (const auto& [id, node] : metadata.nodes) {
        if (node.status != MetadataNodeStatus::kRemoved) {
            builder.addNode(id, node.client.host, node.client.port);
        }
    }
    for (const auto& [id, group] : metadata.groups) {
        builder.addShard(id, group.voters);
    }
    for (std::size_t slot = 0; slot < metadata.slots.size(); ++slot) {
        const MetadataSlotRecord& owner = metadata.slots[slot];
        if (owner.active_group == kNoShard) {
            continue;
        }
        builder.assignSlot(owner.active_group, static_cast<SlotId>(slot));
        builder.setSlotEpoch(static_cast<SlotId>(slot), owner.ownership_epoch);
    }

    for (const auto& [id, migration] : metadata.migrations) {
        (void)id;
        if (migration.phase == MigrationPhase::kMetadataCommitted ||
            migration.phase == MigrationPhase::kCleanup ||
            migration.phase == MigrationPhase::kAborting ||
            migration.phase == MigrationPhase::kAborted) {
            continue;
        }
        builder.setMigration(
            migration.slot, migration.source, migration.target,
            migration.phase == MigrationPhase::kTargetActiveAsk,
            migration.from_epoch, migration.to_epoch);
    }

    if (previous) {
        for (const auto& [id, group] : metadata.groups) {
            const LeaderHint* hint = previous->leaderHint(id);
            if (hint != nullptr &&
                std::find(group.voters.begin(), group.voters.end(),
                          hint->node_id) != group.voters.end() &&
                metadata.nodes.find(hint->node_id) != metadata.nodes.end()) {
                builder.setLeaderHint(id, hint->node_id, hint->term,
                                      hint->expires_at_ms);
            }
        }
    }
    return builder.build();
}

void MetadataWatcher::addRouter(ClusterRouter& router) {
    if (std::find(routers_.begin(), routers_.end(), &router) == routers_.end()) {
        routers_.push_back(&router);
    }
}

bool MetadataWatcher::onCommitted(const ClusterMetadata& metadata, std::string* error) {
    if (!metadata.initialized() || metadata.revision <= last_revision_) {
        if (error != nullptr) {
            *error = metadata.initialized() ? "stale metadata revision"
                                            : "metadata is not initialized";
        }
        return false;
    }
    try {
        TopologyPtr previous;
        if (!routers_.empty()) previous = routers_.front()->topology();
        const TopologyPtr snapshot = buildTopologySnapshot(metadata, previous);
        for (ClusterRouter* router : routers_) {
            router->publishTopology(snapshot);
        }
        last_revision_ = metadata.revision;
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::invalid_argument& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

} // namespace cluster
