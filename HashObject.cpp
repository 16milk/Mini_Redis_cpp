#include "HashObject.hpp"

// HashObject 的实现
HashObject::HashObject()
    : RedisObject(ObjectType::HASH)
    , encoding_(ObjectEncoding::ZIPLIST)
    , storage_(std::vector<std::pair<std::string, std::string>>{}) 
{}

// 判断是否应保持 ziplist
bool HashObject::should_use_ziplist() const {
    if (encoding_ != ObjectEncoding::ZIPLIST) return false;
    const auto& zl = get_ziplist();
    if (zl.size() >= ZIPLIST_MAX_ENTRIES) return false;
    // 检查是否有 field 或 value 太大
    for (const auto& kv : zl) {
        if (kv.first.size() > ZIPLIST_MAX_ENTRY_SIZE ||
            kv.second.size() > ZIPLIST_MAX_ENTRY_SIZE) {
            return false;
        }
    }
    return true;
}

// 升级到 hashtable
void HashObject::promote_to_hashtable() {
    if (encoding_ != ObjectEncoding::ZIPLIST) return;

    Dict new_dict;
    for (const auto& kv : get_ziplist()) {
        new_dict.set_field(kv.first, kv.second);
    }

    storage_ = std::move(new_dict);
    encoding_ = ObjectEncoding::HASHTABLE;
}

//实现 ziplist 查找辅助函数
auto HashObject::find_in_ziplist(const std::string& field)
    -> std::vector<std::pair<std::string, std::string>>::iterator {
    auto& zl = get_ziplist();
    return std::find_if(zl.begin(), zl.end(),
        [&](const auto& kv) { return kv.first == field; });
}

auto HashObject::find_in_ziplist(const std::string& field) const
    -> std::vector<std::pair<std::string, std::string>>::const_iterator {
    const auto& zl = get_ziplist();
    return std::find_if(zl.begin(), zl.end(),
        [&](const auto& kv) { return kv.first == field; });
}

void HashObject::set_field(std::string field, std::string value) {
    // 检查新 field/value 是否太大
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        if (field.size() > ZIPLIST_MAX_ENTRY_SIZE ||
            value.size() > ZIPLIST_MAX_ENTRY_SIZE) {
            promote_to_hashtable();
        }
    }

    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto it = find_in_ziplist(field);
        if (it != get_ziplist().end()) {
            // 更新 existing
            it->second = std::move(value);
        } else {
            // 新增
            get_ziplist().emplace_back(std::move(field), std::move(value));
            // 插入后检查是否超标
            if (!should_use_ziplist()) {
                promote_to_hashtable();
            }
        }
    } else {
        get_hashtable().set_field(std::move(field), std::move(value)); 
    }
}

bool HashObject::get_field(const std::string& field, std::string& out_value) const {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto it = find_in_ziplist(field);
        if (it != get_ziplist().end()) {
            out_value = it->second;
            return true;
        }
    } else {
        auto& ht = get_hashtable();
        if (get_field(field, out_value)) {
            return true;
        }
    }
    return false;
}

size_t HashObject::size() const {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        return get_ziplist().size();
    } else {
        return get_hashtable().size();
    }
}

bool HashObject::del_field(const std::string& field) {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        auto it = find_in_ziplist(field);
        if (it != get_ziplist().end()) {
            get_ziplist().erase(it);
            return true;
        }
    } else {
        return get_hashtable().del_field(field) > 0;
    }
    return false;
}

bool HashObject::exists(const std::string& field) const {
    if (encoding_ == ObjectEncoding::ZIPLIST) {
        return find_in_ziplist(field) != get_ziplist().end();
    } else {
        string out_value;
        return get_hashtable().get_field(field, out_value);
    }
}

void HashObject::try_rehash_for_ms(int ms) {
    if (encoding_ == ObjectEncoding::HASHTABLE) {
        get_hashtable().try_rehash_for_ms(ms);
    }
}