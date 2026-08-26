#pragma once

#include "mini_redis/core/Expiration.hpp"
#include "mini_redis/core/RedisObject.hpp"

#include <memory>
#include <string>
#include <unordered_map>

using ObjectMap = std::unordered_map<std::string, std::shared_ptr<RedisObject>>;

struct RdbLoadResult {
    ObjectMap objects;
    ExpireMap expires;
};

class RdbEncoder {
public:
    static bool saveToFile(
        const std::string& filename,
        const ObjectMap& objects,
        const ExpireMap& expires,
        UnixMillis snapshot_now_ms);

    // Compatibility overload for snapshots without expiration metadata.
    static bool saveToFile(
        const std::string& filename,
        const ObjectMap& objects);

    static RdbLoadResult loadFromFile(
        const std::string& filename,
        UnixMillis load_now_ms);

    // Compatibility overload that returns only objects and uses the current system time.
    static ObjectMap loadFromFile(
        const std::string& filename);

    // The most recent persistence failure on the calling thread. Empty after success.
    static const std::string& lastError();
};

class RdbDecoder {
public:
    explicit RdbDecoder(std::string filename);
    RdbLoadResult decodeAll(UnixMillis load_now_ms);

    // Compatibility overload that returns only objects and uses the current system time.
    ObjectMap decodeAll();

private:
    std::string filename_;
};
