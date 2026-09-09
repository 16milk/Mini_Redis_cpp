#pragma once

#include "mini_redis/core/RedisObject.hpp"

#include <memory>
#include <string>

namespace persistence {

// Logical encoding of a Redis object: type tag plus sorted fields/members.
// Snapshot and migration both persist this form so a restart does not depend
// on whichever compact in-memory encoding a replica happened to be using.
std::string encodeLogicalObject(const RedisObject& object);
std::shared_ptr<RedisObject> decodeLogicalObject(const std::string& bytes,
                                                 std::string& error);

}  // namespace persistence
