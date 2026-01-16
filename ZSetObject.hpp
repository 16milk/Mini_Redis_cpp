#pragma once
#include "RedisObject.hpp"
#include <string>
#include <unordered_map>
#include <memory>


struct ZSetMemberCompare {
    bool operator()(const std::pair<double, std::string>& a,
                    const std::pair<double, std::string>& b) const {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second; // score 相同时按 member 字典序
    }
};

// 有序集合对象
class ZSetObject : public RedisObject {
public:
    ZSetObject();

    // 添加或更新 member 的 score
    void add(double score, const std::string& member);

    // 删除 member
    bool remove(const std::string& member);

    // 获取 member 的 score
    bool get_score(const std::string& member, double& out_score) const;

    // 按 score 范围获取 members（升序）
    std::vector<std::string> range_by_score(double min, double max) const;

    // 获取排名（从 0 开始）
    bool rank(const std::string& member, size_t& out_rank) const;

    size_t size() const;

private:
    std::unordered_map<std::string, double> score_map_;           // member -> score
    std::set<std::pair<double, std::string>, ZSetMemberCompare> sorted_set_; // (score, member)
};