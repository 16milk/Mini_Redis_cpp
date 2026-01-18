// Rdb.cpp
#include "Rdb.hpp"
#include "StringObject.hpp"   
#include "HashObject.hpp" 
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>


// RDB 类型常量
constexpr uint8_t RDB_TYPE_STRING = 0;
constexpr uint8_t RDB_TYPE_HASH   = 2;
constexpr uint8_t RDB_OPCODE_EOF  = 0xFF;
constexpr uint8_t RDB_OPCODE_DB   = 0xFE;

void RdbEncoder::writeMagic(std::ofstream& out) {
    out.write("REDIS", 5);
    out.write("0009", 4); // RDB version 9
}

void RdbEncoder::writeLen(std::ofstream& out, uint64_t len) {
    if (len < (1 << 6)) {
        uint8_t b = (len & 0x3F);
        out.write(reinterpret_cast<char*>(&b), 1);
    } else if (len < (1 << 14)) {
        uint16_t b = (1 << 6) | (len & 0x3FFF);
        b = htole16(b);
        out.write(reinterpret_cast<char*>(&b), 2);
    } else {
        // 简化：暂不支持超大字符串（>16KB）
        throw std::runtime_error("RDB: large string not supported in stage5B");
    }
}

void RdbEncoder::writeString(std::ofstream& out, const std::string& s) {
    writeLen(out, s.size());
    out.write(s.data(), s.size());
}

void RdbEncoder::writeDatabaseHeader(std::ofstream& out, int db_number) {
    out.put(RDB_OPCODE_DB);
    writeLen(out, db_number);
    // key count 和 expire time 可省略（Redis 允许）
}

void RdbEncoder::writeKeyValuePair(std::ofstream& out, const std::string& key, RedisObject* obj) {
    if (obj->type() == ObjectType::STRING) {
        out.put(RDB_TYPE_STRING);
        writeString(out, key);
        auto str_obj = static_cast<StringObject*>(obj);
        writeString(out, str_obj->value());
    } else if (obj->type() == ObjectType::HASH) {
        out.put(RDB_TYPE_HASH);
        writeString(out, key);
        auto hash_obj = static_cast<HashObject*>(obj);
        writeLen(out, hash_obj->size());
        for (const auto& [field, value] : hash_obj->get_all_fields()) {
            writeString(out, field);
            writeString(out, value);
        }
    } else {
        throw std::runtime_error("Unsupported RDB type");
    }
}

void RdbEncoder::writeEOF(std::ofstream& out) {
    out.put(RDB_OPCODE_EOF);
}

void RdbEncoder::writeChecksum(std::ofstream& out) {
    // 阶段五 B：跳过 CRC64，写 8 字节 0
    uint64_t zero = 0;
    out.write(reinterpret_cast<char*>(&zero), 8);
}

bool RdbEncoder::saveToFile(const std::string& filename,
                           const std::unordered_map<std::string, std::shared_ptr<RedisObject>>& data) {
    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;

    try {
        writeMagic(out);
        writeDatabaseHeader(out, 0);

        for (const auto& [key, obj] : data) {
            writeKeyValuePair(out, key, obj.get());
        }

        writeEOF(out);
        writeChecksum(out);
        out.close();
        return true;
    } catch (const std::exception& e) {
        out.close();
        return false;
    }
}

// Rdb.cpp（全局函数）
std::unordered_map<std::string, std::shared_ptr<RedisObject>> RdbEncoder::loadFromFile(const std::string& filename) {
    try {
        RdbDecoder decoder(filename);
        return decoder.decodeAll();
    } catch (const std::exception& e) {
        // 加载失败（文件不存在/损坏），返回空 map
        return {};
    }
}

RdbDecoder::RdbDecoder(const std::string& filename) : in_(filename, std::ios::binary) {
    if (!in_) {
        throw std::runtime_error("Cannot open RDB file");
    }
}

std::unordered_map<std::string, std::shared_ptr<RedisObject>> RdbDecoder::decodeAll() {
    readMagic();
    skipToDatabase(); // 跳过其他 DB（只处理 DB 0）
    return readKeyValues();
}

void RdbDecoder::readExact(char* buf, size_t len) {
    if (!in_.read(buf, len)) {
        throw std::runtime_error("Unexpected EOF in RDB");
    }
}

std::string RdbDecoder::readMagic() {
    char magic[9];
    readExact(magic, 9);
    if (std::string(magic, 5) != "REDIS") {
        throw std::runtime_error("Invalid RDB magic");
    }
    std::string version(magic + 5, 4);
    if (version != "0009") {
        throw std::runtime_error("Unsupported RDB version: " + version);
    }
    return version;
}

uint64_t RdbDecoder::readLen() {
    uint8_t byte;
    readExact(reinterpret_cast<char*>(&byte), 1);
    if ((byte & 0xC0) == 0) {
        return byte & 0x3F;
    } else if ((byte & 0xC0) == 0x40) {
        uint16_t len = byte & 0x3F;
        readExact(reinterpret_cast<char*>(&len) + 1, 1); // 读第二个字节
        len = le16toh(len);
        return len;
    } else {
        throw std::runtime_error("RDB large length not supported");
    }
}

std::string RdbDecoder::readString() {
    uint64_t len = readLen();
    std::string s(len, '\0');
    if (len > 0) {
        readExact(&s[0], len);
    }
    return s;
}

void RdbDecoder::skipToDatabase() {
    while (true) {
        uint8_t opcode;
        readExact(reinterpret_cast<char*>(&opcode), 1);
        if (opcode == RDB_OPCODE_DB) {
            uint64_t db_num = readLen();
            if (db_num == 0) return; // 只处理 DB 0
            // 否则继续跳（简化：假设只有一个 DB）
        } else if (opcode == RDB_OPCODE_EOF) {
            in_.seekg(-1, std::ios::cur); // 回退，让 readKeyValues 处理 EOF
            return;
        } else {
            // 遇到 key-value？说明无 DB header，回退并退出
            in_.seekg(-1, std::ios::cur);
            return;
        }
    }
}

std::unordered_map<std::string, std::shared_ptr<RedisObject>> RdbDecoder::readKeyValues() {
    std::unordered_map<std::string, std::shared_ptr<RedisObject>> data;
    while (true) {
        uint8_t type;
        readExact(reinterpret_cast<char*>(&type), 1);
        if (type == RDB_OPCODE_EOF) {
            break;
        }

        std::string key = readString();
        std::shared_ptr<RedisObject> obj;

        if (type == RDB_TYPE_STRING) {
            std::string value = readString();
            obj = std::make_shared<StringObject>(std::move(value));
        } else if (type == RDB_TYPE_HASH) {
            uint64_t field_count = readLen();
            auto hash_obj = std::make_shared<HashObject>();
            for (uint64_t i = 0; i < field_count; ++i) {
                std::string field = readString();
                std::string value = readString();
                hash_obj->set_field(field, value);
            }
            obj = hash_obj;
        } else {
            throw std::runtime_error("Unsupported RDB type during load: " + std::to_string(type));
        }

        data.emplace(std::move(key), std::move(obj));
    }
    // 跳过 checksum（8 字节）
    in_.ignore(8);
    return data;
}