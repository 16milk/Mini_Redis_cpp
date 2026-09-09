#pragma once

#include "mini_redis/cluster/Metadata.hpp"
#include "mini_redis/cluster/Router.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cluster {

TopologyPtr buildTopologySnapshot(const ClusterMetadata& metadata,
                                  const TopologyPtr& previous = {});

// Consumes committed metadata only. All registered reactors receive the same
// immutable snapshot; stale revisions are ignored.
class MetadataWatcher {
public:
    MetadataWatcher() = default;
    explicit MetadataWatcher(ClusterRouter& router) { addRouter(router); }

    void addRouter(ClusterRouter& router);
    bool onCommitted(const ClusterMetadata& metadata, std::string* error = nullptr);
    std::uint64_t lastRevision() const { return last_revision_; }

private:
    std::vector<ClusterRouter*> routers_;
    std::uint64_t last_revision_ = 0;
};

} // namespace cluster
