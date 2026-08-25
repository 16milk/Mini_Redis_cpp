#pragma once

#include "mini_redis/core/RedisObject.hpp"

#include <memory>
#include <string>
#include <unordered_map>

class RdbEncoder {
public:
    static bool saveToFile(
        const std::string& filename,
        const std::unordered_map<std::string, std::shared_ptr<RedisObject>>& data);

    static std::unordered_map<std::string, std::shared_ptr<RedisObject>> loadFromFile(
        const std::string& filename);

    // The most recent persistence failure on the calling thread. Empty after success.
    static const std::string& lastError();
};

class RdbDecoder {
public:
    explicit RdbDecoder(std::string filename);
    std::unordered_map<std::string, std::shared_ptr<RedisObject>> decodeAll();

private:
    std::string filename_;
};
