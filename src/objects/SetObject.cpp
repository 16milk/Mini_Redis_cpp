#include "mini_redis/objects/SetObject.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <errno.h>

SetObject::SetObject()
    : RedisObject(ObjectType::SET)
    , encoding_(SetEncoding::INTSET)
    , storage_(intset{}) {}

std::optional<int64_t> SetObject::tryParseInt(const std::string& s) {
    if (s.empty()) return std::nullopt;

    size_t start = 0;
    if (s[0] == '-') {
        if (s.size() == 1) return std::nullopt;
        start = 1;
    }

    for (size_t i = start; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return std::nullopt;
        }
    }

    char* end;
    const char* cstr = s.c_str();
    errno = 0;
    long long val = std::strtoll(cstr, &end, 10);

    if (errno == ERANGE || end != cstr + s.size()) {
        return std::nullopt;
    }

    const auto result = static_cast<int64_t>(val);
    // 只有无损、规范化的十进制表示才能使用 intset；例如 "01" 是字符串成员。
    return std::to_string(result) == s ? std::optional<int64_t>(result)
                                       : std::nullopt;
}

bool SetObject::should_use_intset() const {
    if (encoding_ != SetEncoding::INTSET) return false;
    return get_intset().size() < INTSET_MAX_ENTRIES;
}

void SetObject::promote_to_strset() {
    if (encoding_ != SetEncoding::INTSET) return;

    std::unordered_set<std::string> new_set;
    const auto& is = get_intset();
    new_set.reserve(is.size());
    for (int64_t v : is.data()) {
        new_set.emplace(std::to_string(v));
    }

    storage_ = std::move(new_set);
    encoding_ = SetEncoding::STRSET;
}

bool SetObject::add(const std::string& member) {
    if (encoding_ == SetEncoding::INTSET) {
        auto intval = tryParseInt(member);
        if (intval.has_value()) {
            if (get_intset().contains(*intval)) {
                return false;
            }
            if (should_use_intset()) {
                return get_intset().insert(*intval);
            }
            // 超过阈值，先升级再插入。
            promote_to_strset();
            return get_strset().insert(member).second;
        } else {
            // 非整数，必须升级
            promote_to_strset();
            return get_strset().insert(member).second;
        }
    } else {
        return get_strset().insert(member).second;
    }
}

bool SetObject::remove(const std::string& member) {
    if (encoding_ == SetEncoding::INTSET) {
        auto intval = tryParseInt(member);
        if (intval.has_value()) {
            return get_intset().erase(*intval);
        }
        return false;
    } else {
        return get_strset().erase(member) > 0;
    }
}

bool SetObject::contains(const std::string& member) const {
    if (encoding_ == SetEncoding::INTSET) {
        auto intval = tryParseInt(member);
        if (intval.has_value()) {
            return get_intset().contains(*intval);
        }
        return false;
    } else {
        return get_strset().count(member) > 0;
    }
}

size_t SetObject::size() const {
    if (encoding_ == SetEncoding::INTSET) {
        return get_intset().size();
    }
    return get_strset().size();
}

std::vector<std::string> SetObject::members() const {
    std::vector<std::string> result;
    if (encoding_ == SetEncoding::INTSET) {
        const auto& is = get_intset();
        result.reserve(is.size());
        for (int64_t v : is.data()) {
            result.push_back(std::to_string(v));
        }
    } else {
        const auto& ss = get_strset();
        result.assign(ss.begin(), ss.end());
    }
    std::sort(result.begin(), result.end());
    return result;
}

size_t SetObject::memory_usage() const {
    size_t base = sizeof(SetObject);
    if (encoding_ == SetEncoding::INTSET) {
        const auto& is = get_intset();
        base += is.size() * sizeof(int64_t);
    } else {
        const auto& ss = get_strset();
        for (const auto& s : ss) {
            base += sizeof(std::string) + s.capacity();
        }
    }
    return base;
}
