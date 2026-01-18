#include "Dict.hpp"

// Dict
Dict::Dict() {
    ht_[0].resize(INIT_HT_SIZE);
    ht_[1].clear(); // 空
    rehashidx_ = -1;
}

// 生产环境可用 SipHash，但用 std::hash 足够。
size_t Dict::hash_key(const std::string& key) const {
    std::hash<std::string> hasher;
    return hasher(key) % ht_[0].size();
}

void Dict::set_field(std::string key, std::string value) {
    // 如果正在 rehash，先迁移一个 bucket
    if (is_rehashing()) {
        rehash_step(1);
    }

    size_t idx = hash_key(key);
    auto& head = ht_[0][idx];

    // 查找是否已存在
    for (auto* p = head.get(); p; p = p->next.get()) {
        if (p->key == key) {
            p->value = std::move(value);
            return;
        }
    }

    // 不存在，插入新节点（头插）
    auto new_entry = std::make_unique<HashEntry>(std::move(key), std::move(value));
    new_entry->next = std::move(head);
    head = std::move(new_entry);
    used_++;

    // 检查是否需要扩容
    expand_if_needed();
}

// 扩容检查
void Dict::expand_if_needed() {
    if (is_rehashing()) return;
    if (ht_[0].empty()) return;

    // 负载因子 > 1 时扩容（Redis 默认）
    if (used_ >= static_cast<long long>(ht_[0].size())) {
        size_t new_size = ht_[0].size() * 2;
        ht_[1].resize(new_size);
        rehashidx_ = 0; // 开始 rehash
    }
}

bool Dict::get_field(const std::string& key, std::string& out_value) const {
    // 注意：const 函数不能修改 rehashidx_，所以不能主动 rehash_step
    // 但 Redis 在读操作也会推进 rehash，我们这里简化：不推进（或可加 mutable）
    // 为简单，假设调用者会在非 const 操作中推进
    // rehash 期间，key 可能在 ht[0] 或 ht[1]，所以要查两张表。
    for (int table = 0; table <= 1; ++table) {
        if (ht_[table].empty()) continue;
        size_t idx = std::hash<std::string>{}(key) % ht_[table].size();
        for (auto* p = ht_[table][idx].get(); p; p = p->next.get()) {
            if (p->key == key) {
                out_value = p->value;
                return true;
            }
        }
        if (!is_rehashing()) break; // 只查 ht[0]
    }
    return false;
}

bool Dict::del_field(const std::string& key) {
    if (is_rehashing()) {
        rehash_step(1);
    }

    for (int table = 0; table <= 1; ++table) {
        if (ht_[table].empty()) continue;
        size_t idx = std::hash<std::string>{}(key) % ht_[table].size();
        auto& head = ht_[table][idx];
        HashEntry* prev = nullptr;
        for (auto* p = head.get(); p; p = p->next.get()) {
            if (p->key == key) {
                // 删除 p
                if (prev) {
                    prev->next = std::move(p->next);
                } else {
                    head = std::move(p->next);
                }
                used_--;
                shrink_if_needed();
                return true;
            }
            prev = p;
        }
        if (!is_rehashing()) break;
    }
    return false;
}

// 缩容检查
void Dict::shrink_if_needed() {
    if (is_rehashing()) return;
    if (ht_[0].size() <= INIT_HT_SIZE) return;
    if (used_ * 100 / ht_[0].size() < HASHTABLE_MIN_FILL) {
        size_t new_size = std::max(INIT_HT_SIZE, ht_[0].size() / 2);
        ht_[1].resize(new_size);
        rehashidx_ = 0;
    }
}

// 实现 渐进式 rehash,每次只迁移一个 bucket（或限制空 bucket 跳跃次数）。
int Dict::rehash_step(int n) {
    if (!is_rehashing()) return 0;
    if (ht_[0].empty()) {
        rehashidx_ = -1;
        return 0;
    }

    int empty_visits = 0;
    while (n-- && used_ > 0) {
        // 跳过空 bucket（最多跳 10 * n 次）
        while (rehashidx_ < static_cast<long long>(ht_[0].size()) &&
               ht_[0][rehashidx_].get() == nullptr) {
            rehashidx_++;
            empty_visits++;
            if (empty_visits > 10 * n) return 1;
        }

        if (rehashidx_ >= static_cast<long long>(ht_[0].size())) {
            // rehash 完成
            ht_[0] = std::move(ht_[1]);
            ht_[1].clear();
            rehashidx_ = -1;
            return 0;
        }

        // 迁移整个 bucket
        auto& old_bucket = ht_[0][rehashidx_];
        while (old_bucket) {
            auto entry = std::move(old_bucket);
            old_bucket = std::move(entry->next);

            // 插入 ht[1]
            size_t new_idx = std::hash<std::string>{}(entry->key) % ht_[1].size();
            entry->next = std::move(ht_[1][new_idx]);
            ht_[1][new_idx] = std::move(entry);
        }
        rehashidx_++;
    }
    return 1; // 仍在 rehash
}

void Dict::try_rehash_for_ms(int ms) {
    if (!is_rehashing()) return;

    auto start = Clock::now();
    auto end = start + std::chrono::milliseconds(ms);

    int batch = 100; // 每次迁移 100 个 bucket（可调）
    while (is_rehashing()) {
        rehash_step(batch);
        if (Clock::now() >= end) break;
    }
}