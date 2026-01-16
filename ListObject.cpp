#include "ListObject.hpp"
#include "RedisObject.hpp"
#include <algorithm>
#include <optional>

ListObject::ListObject() 
    : RedisObject(ObjectType::LIST)
    , encoding_(ObjectEncoding::ZIPLIST)
    , storage_(std::vector<std::string>{}) 
{}

// 索引归一化（独立于存储类型）
std::optional<size_t> ListObject::normalize_index(int index, size_t sz) const {
    if (sz == 0) return std::nullopt;
    long long idx = index;
    if (idx < 0) idx += sz;
    if (idx < 0 || idx >= static_cast<long long>(sz)) return std::nullopt;
    return static_cast<size_t>(idx);
}

// 判断是否还能用 ziplist
bool ListObject::should_use_ziplist() const {
    if (encoding_ != ObjectEncoding::ZIPLIST) return false;
    const auto& zl = get_ziplist();
    if (zl.size() >= ZIPLIST_MAX_ENTRIES) return false;
    // 可选：检查是否有元素太大
    // for (const auto& s : zl) {
    //     if (s.size() > ZIPLIST_MAX_ENTRY_SIZE) return false;
    // }
    // 为简化，先只检查数量
    return true;
}

// 升级！关键！
void ListObject::promote_to_linkedlist() {
    if (encoding_ != ObjectEncoding::ZIPLIST) return;

    const auto& old_zl = get_ziplist();
    std::deque<std::string> new_ll(old_zl.begin(), old_zl.end());
    
    // 替换 storage_
    storage_ = std::move(new_ll);
    encoding_ = ObjectEncoding::LINKEDLIST;
}

//实现 getter（安全访问 variant）
std::vector<std::string>& ListObject::get_ziplist() {
    return std::get<std::vector<std::string>>(storage_);
}

std::deque<std::string>& ListObject::get_linkedlist() {
    return std::get<std::deque<std::string>>(storage_);
}

const std::vector<std::string>& ListObject::get_ziplist() const {
    return std::get<std::vector<std::string>>(storage_);
}

const std::deque<std::string>& ListObject::get_linkedlist() const {
    return std::get<std::deque<std::string>>(storage_);
}

void ListObject::push_front(std::string value) {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        // 检查插入后是否超标
        if (get_ziplist().size() + 1 > ZIPLIST_MAX_ENTRIES ||
            value.size() > ZIPLIST_MAX_ENTRY_SIZE) {
            promote_to_linkedlist();
        }
    }

    if (encoding_ == ObjectEncoding::ZIPLIST) {
        get_ziplist().push_front(std::move(value));
    } else {
        get_linkedlist().push_front(std::move(value));
    }
}

void ListObject::push_back(std::string value) {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        // 检查插入后是否超标
        if (get_ziplist().size() + 1 > ZIPLIST_MAX_ENTRIES ||
            value.size() > ZIPLIST_MAX_ENTRY_SIZE) {
            promote_to_linkedlist();
        }
    }

    if (encoding_ == ObjectEncoding::ZIPLIST) {
        get_ziplist().push_back(std::move(value));
    } else {
        get_linkedlist().push_back(std::move(value));
    }
}

bool ListObject::pop_front(std::string& out) {
    if (size() == 0) return false;
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto& zl = get_ziplist();
        out = std::move(zl.front());
        zl.erase(zl.begin());
    } else {
        auto& ll = get_linkedlist();
        out = std::move(ll.front());
        ll.pop_front();
    }
    return true;
}

bool ListObject::pop_back(std::string& out) {
    if (size() == 0) return false;
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto& zl = get_ziplist();
        out = std::move(zl.back());
        zl.pop_back();
    } else {
        auto& l1 = get_linkedlist();
        out = std::move(l1.back());
        l1.pop_back();
    }
    return true;
}

std::optional<size_t> ListObject::normalize_index(int index) const {
    if (list_.empty()) return std::nullopt;
    long long sz = static_cast<long long>(list_.size());
    long long idx = index;
    if (idx < 0) idx = sz + idx;
    if (idx < 0 || idx >= sz) return std::nullopt;
    return static_cast<size_t>(idx);
}

// 将 [start, stop] 转为有效正索引范围 [l, r]（闭区间），若无效返回 nullopt
std::optional<std::pair<size_t, size_t>> 
ListObject::normalize_range(int start, int stop, size_t sz) const {
    if (sz == 0) return std::nullopt;

    auto norm = [sz](int idx) -> long long {
        if (idx < 0) idx += sz;
        if (idx < 0) return -1;
        if (idx >= static_cast<long long>(sz)) return sz;
        return idx;
    };

    long long l = norm(start);
    long long r = norm(stop);

    if (l == -1) l = 0;
    if (r >= static_cast<long long>(sz)) r = sz - 1;

    if (l > r || l >= static_cast<long long>(sz)) {
        return std::nullopt; // 无效范围 → 清空
    }
    return std::make_pair(static_cast<size_t>(l), static_cast<size_t>(r));
}

bool ListObject::get(int index, std::string& out) const {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        const auto& zl = get_ziplist();
        auto idx = normalize_index(index, zl.size());
        if (!idx) return false;
        out = zl[*idx];
        return true;
    } else {
        const auto& ll = get_linkedlist();
        auto idx = normalize_index(index, ll.size());
        if (!idx) return false;
        out = ll[*idx];
        return true;
    }
}

long long ListObject::remove(long long count, const std::string& value) {
    if (size() == 0) return 0;

    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto& zl = get_ziplist();
        long long removed = 0;

        if (count > 0) {
            // 从前往后
            for (auto it = zl.begin(); it != zl.end() && removed < count; ) {
                if (*it == value) {
                    it = zl.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        } else if (count < 0) {
            // 从后往前
            count = -count;
            for (auto it = zl.rbegin(); it != zl.rend() && removed < count; ) {
                if (*it == value) {
                    // erase by base: riter.base() is next forward iterator
                    it = std::reverse_iterator(zl.erase((++it).base()));
                    ++removed;
                } else {
                    ++it;
                }
            }
        } else { // count == 0
            // 删除所有
            for (auto it = zl.begin(); it != zl.end(); ) {
                if (*it == value) {
                    it = zl.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        return removed;

    } else {
        // LINKEDLIST 分支
        auto& ll = get_linkedlist();
        long long removed = 0;

        if (count > 0) {
            for (auto it = ll.begin(); it != ll.end() && removed < count; ) {
                if (*it == value) {
                    it = ll.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        } else if (count < 0) {
            count = -count;
            for (auto it = ll.rbegin(); it != ll.rend() && removed < count; ) {
                if (*it == value) {
                    it = std::reverse_iterator(ll.erase((++it).base()));
                    ++removed;
                } else {
                    ++it;
                }
            }
        } else {
            for (auto it = ll.begin(); it != ll.end(); ) {
                if (*it == value) {
                    it = ll.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        return removed;
    }
}

void ListObject::trim(int start, int stop) {
    size_t sz = size();
    if (sz == 0) return;

    auto range_opt = normalize_range(start, stop, sz);
    if (!range_opt) {
        // 无效范围 → 清空
        if (encoding_ == ObjectEncoding::ZIPLIST) {
            get_ziplist().clear();
        } else {
            get_linkedlist().clear();
        }
        return;
    }

    auto [l, r] = *range_opt; // l <= r, both in [0, sz)

    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto& zl = get_ziplist();
        std::vector<std::string> new_zl;
        new_zl.assign(zl.begin() + l, zl.begin() + r + 1);
        zl = std::move(new_zl);
    } else {
        auto& ll = get_linkedlist();
        std::deque<std::string> new_ll;
        new_ll.assign(ll.begin() + l, ll.begin() + r + 1);
        ll = std::move(new_ll);
    }
}

bool ListObject::insert(const std::string& pivot, const std::string& value, InsertPosition pos) {
    // 如果是 ziplist 且 value 太大，先升级
    if (encoding_ == ObjectEncoding::ZIPLIST && value.size() > ZIPLIST_MAX_ENTRY_SIZE) {
        promote_to_linkedlist();
    }

    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto& zl = get_ziplist();
        auto it = std::find(zl.begin(), zl.end(), pivot);
        if (it == zl.end()) return false;
        if (pos == BEFORE) {
            zl.insert(it, value);
        } else {
            zl.insert(std::next(it), value);
        }
        // 插入后检查是否超标
        if (zl.size() > ZIPLIST_MAX_ENTRIES) {
            promote_to_linkedlist();
        }
    } else {
        // linkedlist 分支（略，类似之前实现）
    }
    return true;
}

size_t ListObject::size() const {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        return get_ziplist().size();
    } else {
        return get_linkedlist().size();
    }
}
