#include "HashObject.hpp"

// HashObject 的实现
HashObject::HashObject() : RedisObject(ObjectType::HASH) {}

void HashObject::set_field(const std::string& field, const std::string& value) {
    fields_[field] = value;
}

bool HashObject::get_field(const std::string& field, std::string& out_value) const {
    auto it = fields_.find(field);
    if (it != fields_.end()) {
        out_value = it->second;
        return true;
    }
    return false;
}

const std::unordered_map<std::string, std::string>& HashObject::getFields() const {
    return fields_;
}

size_t HashObject::size() const {
    return fields_.size();
}