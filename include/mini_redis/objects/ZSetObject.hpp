#pragma once

#include "mini_redis/core/RedisObject.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct ZSetMemberCompare {
    bool operator()(const std::pair<double, std::string>& left,
                    const std::pair<double, std::string>& right) const {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    }
};

class ZSetObject : public RedisObject {
public:
    ZSetObject();

    ObjectType type() const override { return ObjectType::ZSET; }
    size_t memory_usage() const override;

    // 返回 true 表示新成员，false 表示更新既有成员。
    bool add(double score, const std::string& member);
    bool remove(const std::string& member);
    bool get_score(const std::string& member, double& out_score) const;
    std::vector<std::string> range(long long start, long long stop,
                                   bool with_scores = false) const;
    std::vector<std::string> range_by_score(double min, double max,
                                            bool with_scores = false) const;
    bool rank(const std::string& member, size_t& out_rank) const;
    std::vector<std::pair<std::string, double>> members_with_scores() const;
    size_t size() const;
    static std::string format_score(double score);

private:
    std::unordered_map<std::string, double> score_map_;
    std::set<std::pair<double, std::string>, ZSetMemberCompare> sorted_set_;

};
