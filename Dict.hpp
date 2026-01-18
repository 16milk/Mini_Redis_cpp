// Dict.hpp
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <chrono>

struct HashEntry {
    std::string key;
    std::string value;
    std::unique_ptr<HashEntry> next; // 链地址法

    HashEntry(std::string k, std::string v)
        : key(std::move(k)), value(std::move(v)) {}
};

class Dict {
public:
    Dict();
    ~Dict() = default;

    void set_field(std::string key, std::string value);
    bool get_field(const std::string& key, std::string& out_value) const;
    bool del_field(const std::string& key);
    size_t size() const { return used_; }

    // rehash 相关
    bool is_rehashing() const { return rehashidx_ != -1; }
    void enable_rehash(); // 开始 rehash（分配新表）
    int rehash_step(int n); // 迁移 n 个 bucket

    // 尝试在指定毫秒内推进 rehash
    void try_rehash_for_ms(int ms);

private:
    static const size_t INIT_HT_SIZE = 4;
    static const size_t HASHTABLE_MIN_FILL = 10; // 缩容阈值（used / size < 10%）

    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    std::vector<std::unique_ptr<HashEntry>> ht_[2]; // ht[0] 主表，ht[1] 新表（rehash 时用）
    long long used_ = 0;       // 总元素数
    long long rehashidx_ = -1; // -1 表示未 rehash，否则表示下一个要迁移的 bucket index

    size_t hash_key(const std::string& key) const;
    void expand_if_needed();
    void shrink_if_needed();
    void do_rehash(int n); // 实际迁移逻辑
};