// intset.cpp
#include "intset.hpp"
#include <algorithm> // for lower_bound

bool intset::binary_search(int64_t value, size_t& pos) const {
    auto it = std::lower_bound(data_.begin(), data_.end(), value);
    pos = static_cast<size_t>(it - data_.begin());
    return (it != data_.end() && *it == value);
}

bool intset::insert(int64_t value) {
    size_t pos;
    if (binary_search(value, pos)) {
        return false; // 已存在，不插入
    }
    data_.insert(data_.begin() + pos, value);
    return true;
}

bool intset::erase(int64_t value) {
    size_t pos;
    if (!binary_search(value, pos)) {
        return false; // 不存在
    }
    data_.erase(data_.begin() + pos);
    return true;
}

bool intset::contains(int64_t value) const {
    size_t pos;
    return binary_search(value, pos);
}