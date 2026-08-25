// intset.hpp
#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

class intset {
public:
    intset() = default;

    // 插入一个整数，返回 true 表示成功插入（未重复）
    bool insert(int64_t value);

    // 删除一个整数，返回 true 表示删除成功（存在）
    bool erase(int64_t value);

    // 检查是否包含该整数
    bool contains(int64_t value) const;

    // 获取所有元素（用于升级时遍历）
    const std::vector<int64_t>& data() const noexcept { return data_; }

    // 元素个数
    size_t size() const noexcept { return data_.size(); }

    // 是否为空
    bool empty() const noexcept { return data_.empty(); }

    // 清空集合
    void clear() noexcept { data_.clear(); }

private:
    std::vector<int64_t> data_;  // 保持升序排列

    // 二分查找：若找到返回 true 并设置 pos 为索引；否则 pos 为插入位置
    bool binary_search(int64_t value, size_t& pos) const;
};