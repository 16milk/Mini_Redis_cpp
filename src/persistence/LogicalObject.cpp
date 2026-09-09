#include "mini_redis/persistence/LogicalObject.hpp"

#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include "mini_redis/persistence/Codec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace persistence {
namespace {

enum class ValueKind : std::uint8_t {
    kString = 1,
    kList = 2,
    kSet = 3,
    kHash = 4,
    kZSet = 5,
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

}  // namespace

std::string encodeLogicalObject(const RedisObject& object) {
    ByteWriter writer;
    switch (object.type()) {
        case ObjectType::STRING: {
            writer.u8(static_cast<std::uint8_t>(ValueKind::kString));
            writer.str(static_cast<const StringObject&>(object).value());
            break;
        }
        case ObjectType::LIST: {
            writer.u8(static_cast<std::uint8_t>(ValueKind::kList));
            const std::vector<std::string> values =
                static_cast<const ListObject&>(object).values();
            writer.u32(static_cast<std::uint32_t>(values.size()));
            for (const std::string& value : values) {
                writer.str(value);
            }
            break;
        }
        case ObjectType::SET: {
            writer.u8(static_cast<std::uint8_t>(ValueKind::kSet));
            std::vector<std::string> members = static_cast<const SetObject&>(object).members();
            std::sort(members.begin(), members.end());
            writer.u32(static_cast<std::uint32_t>(members.size()));
            for (const std::string& member : members) {
                writer.str(member);
            }
            break;
        }
        case ObjectType::HASH: {
            writer.u8(static_cast<std::uint8_t>(ValueKind::kHash));
            std::vector<std::pair<std::string, std::string>> fields =
                static_cast<const HashObject&>(object).get_all_fields();
            std::sort(fields.begin(), fields.end());
            writer.u32(static_cast<std::uint32_t>(fields.size()));
            for (const auto& [field, value] : fields) {
                writer.str(field);
                writer.str(value);
            }
            break;
        }
        case ObjectType::ZSET: {
            writer.u8(static_cast<std::uint8_t>(ValueKind::kZSet));
            std::vector<std::pair<std::string, double>> members =
                static_cast<const ZSetObject&>(object).members_with_scores();
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
            writer.u32(static_cast<std::uint32_t>(members.size()));
            for (const auto& [member, score] : members) {
                writer.str(member);
                writer.u64(doubleToBits(score));
            }
            break;
        }
    }
    return writer.finish();
}

std::shared_ptr<RedisObject> decodeLogicalObject(const std::string& bytes, std::string& error) {
    ByteReader reader(bytes);
    std::uint8_t kind = 0;
    if (!reader.u8(kind)) {
        error = "truncated logical object";
        return nullptr;
    }
    std::uint32_t count = 0;
    std::shared_ptr<RedisObject> object;
    switch (static_cast<ValueKind>(kind)) {
        case ValueKind::kString: {
            std::string value;
            if (!reader.str(value)) {
                break;
            }
            object = std::make_shared<StringObject>(std::move(value));
            break;
        }
        case ValueKind::kList: {
            if (!reader.u32(count) || count > kMaxCodecItems) {
                break;
            }
            auto list = std::make_shared<ListObject>();
            bool ok = true;
            for (std::uint32_t index = 0; index < count && ok; ++index) {
                std::string value;
                ok = reader.str(value);
                if (ok) {
                    list->push_back(std::move(value));
                }
            }
            if (ok) {
                object = std::move(list);
            }
            break;
        }
        case ValueKind::kSet: {
            if (!reader.u32(count) || count > kMaxCodecItems) {
                break;
            }
            auto set = std::make_shared<SetObject>();
            bool ok = true;
            for (std::uint32_t index = 0; index < count && ok; ++index) {
                std::string member;
                ok = reader.str(member);
                if (ok) {
                    set->add(member);
                }
            }
            if (ok) {
                object = std::move(set);
            }
            break;
        }
        case ValueKind::kHash: {
            if (!reader.u32(count) || count > kMaxCodecItems) {
                break;
            }
            auto hash = std::make_shared<HashObject>();
            bool ok = true;
            for (std::uint32_t index = 0; index < count && ok; ++index) {
                std::string field;
                std::string value;
                ok = reader.str(field) && reader.str(value);
                if (ok) {
                    hash->set_field(std::move(field), std::move(value));
                }
            }
            if (ok) {
                object = std::move(hash);
            }
            break;
        }
        case ValueKind::kZSet: {
            if (!reader.u32(count) || count > kMaxCodecItems) {
                break;
            }
            auto zset = std::make_shared<ZSetObject>();
            bool ok = true;
            for (std::uint32_t index = 0; index < count && ok; ++index) {
                std::string member;
                std::uint64_t score_bits = 0;
                ok = reader.str(member) && reader.u64(score_bits);
                if (ok) {
                    zset->add(bitsToDouble(score_bits), member);
                }
            }
            if (ok) {
                object = std::move(zset);
            }
            break;
        }
        default:
            error = "unknown logical object kind";
            return nullptr;
    }
    if (!object || !reader.done()) {
        error = "truncated or trailing logical object";
        return nullptr;
    }
    error.clear();
    return object;
}

}  // namespace persistence
