#include "mini_redis/objects/ZSetObject.hpp"

#include <array>
#include <charconv>
#include <system_error>

ZSetObject::ZSetObject() : RedisObject(ObjectType::ZSET) {}

bool ZSetObject::add(double score, const std::string& member) {
    const auto existing = score_map_.find(member);
    if (existing == score_map_.end()) {
        score_map_.emplace(member, score);
        sorted_set_.emplace(score, member);
        return true;
    }

    if (existing->second == score) {
        return false;
    }

    sorted_set_.erase({existing->second, member});
    existing->second = score;
    sorted_set_.emplace(score, member);
    return false;
}

bool ZSetObject::remove(const std::string& member) {
    const auto existing = score_map_.find(member);
    if (existing == score_map_.end()) {
        return false;
    }

    sorted_set_.erase({existing->second, member});
    score_map_.erase(existing);
    return true;
}

bool ZSetObject::get_score(const std::string& member, double& out_score) const {
    const auto existing = score_map_.find(member);
    if (existing == score_map_.end()) {
        return false;
    }

    out_score = existing->second;
    return true;
}

std::string ZSetObject::format_score(double score) {
    if (score == 0) {
        return "0";
    }

    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), score, std::chars_format::general);
    if (error != std::errc{}) {
        return "0"; // 调用方不会传入 NaN 或无穷大。
    }
    return {buffer.data(), end};
}

std::vector<std::string> ZSetObject::range(long long start, long long stop,
                                           bool with_scores) const {
    const size_t count = sorted_set_.size();
    if (count == 0) {
        return {};
    }

    if (start < 0) {
        start += static_cast<long long>(count);
    }
    if (stop < 0) {
        stop += static_cast<long long>(count);
    }
    if (start < 0) {
        start = 0;
    }
    if (stop < 0 || start >= static_cast<long long>(count) || start > stop) {
        return {};
    }
    if (stop >= static_cast<long long>(count)) {
        stop = static_cast<long long>(count) - 1;
    }

    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(stop - start + 1) * (with_scores ? 2 : 1));
    auto it = sorted_set_.begin();
    std::advance(it, start);
    for (long long index = start; index <= stop; ++index, ++it) {
        result.push_back(it->second);
        if (with_scores) {
            result.push_back(format_score(it->first));
        }
    }
    return result;
}

std::vector<std::string> ZSetObject::range_by_score(double min, double max,
                                                    bool with_scores) const {
    if (min > max) {
        return {};
    }

    std::vector<std::string> result;
    const auto begin = sorted_set_.lower_bound({min, ""});
    for (auto it = begin; it != sorted_set_.end() && it->first <= max; ++it) {
        result.push_back(it->second);
        if (with_scores) {
            result.push_back(format_score(it->first));
        }
    }
    return result;
}

bool ZSetObject::rank(const std::string& member, size_t& out_rank) const {
    const auto existing = score_map_.find(member);
    if (existing == score_map_.end()) {
        return false;
    }

    const auto it = sorted_set_.find({existing->second, member});
    out_rank = static_cast<size_t>(std::distance(sorted_set_.begin(), it));
    return true;
}

std::vector<std::pair<std::string, double>> ZSetObject::members_with_scores() const {
    std::vector<std::pair<std::string, double>> result;
    result.reserve(sorted_set_.size());
    for (const auto& [score, member] : sorted_set_) {
        result.emplace_back(member, score);
    }
    return result;
}

size_t ZSetObject::size() const {
    return score_map_.size();
}

size_t ZSetObject::memory_usage() const {
    size_t total = sizeof(*this);
    for (const auto& [member, score] : score_map_) {
        (void)score;
        total += sizeof(member) + member.capacity() + sizeof(double);
    }
    total += sorted_set_.size() * sizeof(std::pair<double, std::string>);
    return total;
}
