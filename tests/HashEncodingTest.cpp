#include "mini_redis/objects/HashObject.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    HashObject hash;
    expect(hash.encoding() == ObjectEncoding::ZIPLIST, "new hash starts as ziplist");
    expect(hash.set_field("field", "value"), "first field is new");
    expect(!hash.set_field("field", "updated"), "existing field update is not new");

    std::string value;
    expect(hash.get_field("field", value) && value == "updated",
           "ziplist update is readable");

    const std::string large_value(HashObject::ZIPLIST_MAX_ENTRY_SIZE + 1, 'x');
    expect(hash.set_field("large", large_value), "large field is new");
    expect(hash.encoding() == ObjectEncoding::HASHTABLE,
           "large value promotes hash to hashtable");
    expect(!hash.set_field("field", "after-promotion"),
           "hashtable update is not new");
    expect(hash.get_field("field", value) && value == "after-promotion",
           "hashtable update is readable");

    HashObject update_promotion_hash;
    expect(update_promotion_hash.set_field("field", "small"),
           "small field is new before update promotion");
    expect(!update_promotion_hash.set_field("field", large_value),
           "large update remains an update");
    expect(update_promotion_hash.encoding() == ObjectEncoding::HASHTABLE,
           "large update promotes hash to hashtable");
    expect(update_promotion_hash.get_field("field", value) && value == large_value,
           "large updated value is preserved after promotion");

    HashObject entry_threshold_hash;
    for (size_t index = 0; index + 1 < HashObject::ZIPLIST_MAX_ENTRIES; ++index) {
        expect(entry_threshold_hash.set_field("field-" + std::to_string(index), "value"),
               "field below entry threshold is new");
    }
    expect(entry_threshold_hash.encoding() == ObjectEncoding::ZIPLIST,
           "hash remains ziplist below entry threshold");
    expect(entry_threshold_hash.set_field("threshold", "value"),
           "threshold field is new");
    expect(entry_threshold_hash.encoding() == ObjectEncoding::HASHTABLE,
           "entry threshold promotes hash to hashtable");

    std::cout << "hash encoding tests passed" << std::endl;
    return EXIT_SUCCESS;
}
