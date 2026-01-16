// Rdb.hpp
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <memory>
#include "RedisObject.hpp"

class RdbEncoder {
public:
    static bool saveToFile(const std::string& filename,
                          const std::unordered_map<std::string, std::shared_ptr<RedisObject>>& data);

    // 加载 RDB 文件，返回 key -> RedisObject 映射
    static std::unordered_map<std::string, std::shared_ptr<RedisObject>> loadFromFile(const std::string& filename);

private:
    static void writeMagic(std::ofstream& out);
    static void writeDatabaseHeader(std::ofstream& out, int db_number = 0);
    static void writeKeyValuePair(std::ofstream& out, const std::string& key, class RedisObject* obj);
    static void writeString(std::ofstream& out, const std::string& s);
    static void writeLen(std::ofstream& out, uint64_t len);
    static void writeEOF(std::ofstream& out);
    static void writeChecksum(std::ofstream& out); // 暂填 0
};

class RdbDecoder {
public:
    explicit RdbDecoder(const std::string& filename);
    std::unordered_map<std::string, std::shared_ptr<RedisObject>> decodeAll();

private:
    void readExact(char* buf, size_t len);
    std::string readMagic();
    uint64_t readLen();
    std::string readString();
    void skipToDatabase();
    std::unordered_map<std::string, std::shared_ptr<RedisObject>> readKeyValues();

private:
    std::ifstream in_;

    // 兼容 le16toh（小端转主机）
#if defined(__linux__)
    // endian.h 已包含
#elif defined(__APPLE__)
    #define le16toh(x) OSSwapLittleToHostInt16(x)
#else
    #define le16toh(x) (x)
#endif

};