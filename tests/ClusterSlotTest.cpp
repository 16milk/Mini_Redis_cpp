#include "mini_redis/cluster/Crc16.hpp"
#include "mini_redis/cluster/Slot.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename T>
void expect_equal(const T& actual, const T& expected, const std::string& description) {
    if (actual != expected) {
        std::cerr << "FAILED: " << description << "\nExpected: " << expected
                  << "\nActual:   " << actual << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// Independent bit-by-bit reference: same polynomial, no lookup table. Any
// mistake in the table-driven implementation shows up as a mismatch.
std::uint16_t referenceCrc16(const std::string& data) {
    std::uint16_t crc = 0;
    for (const char character : data) {
        crc ^= static_cast<std::uint16_t>(static_cast<unsigned char>(character) << 8);
        for (int bit = 0; bit < 8; ++bit) {
            const bool high_bit_set = (crc & 0x8000u) != 0;
            crc = static_cast<std::uint16_t>(crc << 1);
            if (high_bit_set) {
                crc = static_cast<std::uint16_t>(crc ^ 0x1021u);
            }
        }
    }
    return crc;
}

std::string tagOf(const std::string& key) {
    const cluster::HashTag tag = cluster::findHashTag(key);
    return tag.present ? std::string(tag.value) : std::string("<none>");
}

} // namespace

int main() {
    // --- CRC16/XMODEM ---
    expect_equal<std::uint16_t>(cluster::crc16("123456789"), 0x31C3,
                                "CRC16/XMODEM check vector for \"123456789\"");
    expect_equal<std::uint16_t>(cluster::crc16(""), 0,
                                "CRC16 of the empty input is 0");

    // Cross-check the compile-time table against the bitwise reference over a
    // wide range of inputs, including bytes above 0x7f.
    for (int first = 0; first < 256; ++first) {
        for (int second = 0; second < 256; second += 7) {
            std::string sample;
            sample.push_back(static_cast<char>(first));
            sample.push_back(static_cast<char>(second));
            sample.push_back('\0');
            sample.push_back(static_cast<char>(255 - first));
            expect_equal(cluster::crc16(sample), referenceCrc16(sample),
                         "table-driven CRC16 matches the bitwise reference");
        }
    }

    // --- Slot numbers must match a real Redis Cluster node ---
    expect_equal<int>(cluster::keyToSlot("foo"), 12182, "CLUSTER KEYSLOT foo");
    expect_equal<int>(cluster::keyToSlot("bar"), 5061, "CLUSTER KEYSLOT bar");
    expect_equal<int>(cluster::keyToSlot("hello"), 866, "CLUSTER KEYSLOT hello");
    expect_equal<int>(cluster::keyToSlot("mykey"), 14687, "CLUSTER KEYSLOT mykey");
    expect_equal<int>(cluster::keyToSlot(""), 0, "the empty key maps to slot 0");

    for (int value = 0; value < 5000; ++value) {
        const std::string key = "key:" + std::to_string(value);
        const int slot = cluster::keyToSlot(key);
        expect(slot >= 0 && slot < cluster::kSlotCount,
               "every slot number stays inside 0..16383");
    }

    // --- Hash tag extraction ---
    expect_equal(tagOf("{user:42}:profile"), std::string("user:42"),
                 "tag is the content between the braces");
    expect_equal(tagOf("plainkey"), std::string("<none>"),
                 "a key without braces has no tag");
    expect_equal(tagOf("{}foo"), std::string("<none>"),
                 "an empty {} is not a tag, so the whole key is hashed");
    expect_equal(tagOf("{unclosed"), std::string("<none>"),
                 "a missing closing brace means no tag");
    expect_equal(tagOf("foo{}{bar}"), std::string("<none>"),
                 "the first {} wins and no later brace pair is considered");
    expect_equal(tagOf("{{bar}}zap"), std::string("{bar"),
                 "the first '}' after the first '{' closes the tag");
    expect_equal(tagOf("foo{bar}{zap}"), std::string("bar"),
                 "only the first brace pair is used");
    expect_equal(tagOf("}{tag}"), std::string("tag"),
                 "a '}' before the first '{' is ignored");

    // --- Co-location: the point of hash tags ---
    expect_equal<int>(cluster::keyToSlot("{user:42}:profile"), 15880,
                      "{user:42}:profile hashes the tag only");
    expect_equal<int>(cluster::keyToSlot("{user:42}:orders"), 15880,
                      "{user:42}:orders lands in the same slot");
    expect_equal(cluster::keyToSlot("{user:42}:profile"), cluster::keyToSlot("user:42"),
                 "a tagged key hashes exactly like the bare tag");
    expect(cluster::keyToSlot("user:42:profile") != cluster::keyToSlot("user:42:orders"),
           "without a tag the two keys are unrelated");
    expect_equal(cluster::keyToSlot("foo{bar}{zap}"), cluster::keyToSlot("bar"),
                 "later brace pairs do not change the slot");

    // Tags are compared as raw bytes, so embedded NUL is significant.
    const std::string with_nul("{a\0b}x", 6);
    const std::string bare_tag("a\0b", 3);
    expect_equal(cluster::keyToSlot(with_nul), cluster::keyToSlot(bare_tag),
                 "keys are hashed by explicit length, not as C strings");

    // --- Slots are routing units, not Raft groups ---
    for (const std::size_t shard_count : {1u, 2u, 3u, 5u, 16u, 1000u}) {
        const std::vector<cluster::SlotRange> ranges =
            cluster::splitSlotsEvenly(shard_count);
        expect_equal(ranges.size(), shard_count,
                     "one contiguous slot range per logical shard");

        std::size_t covered = 0;
        int expected_start = 0;
        for (const cluster::SlotRange& range : ranges) {
            expect_equal<int>(range.start, expected_start,
                              "slot ranges are contiguous and ascending");
            expect(range.start <= range.end, "slot ranges are non-empty");
            covered += range.count();
            expected_start = range.end + 1;
        }
        expect_equal<int>(expected_start, cluster::kSlotCount,
                          "the ranges end exactly at slot 16383");
        expect_equal<std::size_t>(covered, cluster::kSlotCount,
                                  "all 16384 slots are covered exactly once");
    }

    // Splitting 16384 slots across 3 shards means 3 Raft groups, not 16384.
    const std::vector<cluster::SlotRange> three = cluster::splitSlotsEvenly(3);
    expect_equal<std::size_t>(three.size(), 3,
                              "16384 slots collapse onto 3 logical shards");
    expect_equal<std::size_t>(three[0].count(), 5462, "remainder goes to early shards");
    expect_equal<std::size_t>(three[2].count(), 5461, "later shards take the base width");

    std::cout << "ClusterSlotTest passed" << std::endl;
    return EXIT_SUCCESS;
}
