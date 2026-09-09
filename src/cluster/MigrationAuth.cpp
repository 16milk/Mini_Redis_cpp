#include "mini_redis/cluster/MigrationAuth.hpp"

#include <array>
#include <cstring>

namespace cluster {
namespace {

constexpr std::size_t kBlockBytes = 64;

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

void compress(std::array<std::uint32_t, 8>& state, const unsigned char* block) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
        schedule[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotateRight(schedule[i - 15], 7) ^
                                 rotateRight(schedule[i - 15], 18) ^
                                 (schedule[i - 15] >> 3);
        const std::uint32_t s1 = rotateRight(schedule[i - 2], 17) ^
                                 rotateRight(schedule[i - 2], 19) ^
                                 (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 =
            rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + schedule[i];
        const std::uint32_t s0 =
            rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::string sha256(const std::string& data) {
    std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                          0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                          0x1f83d9abu, 0x5be0cd19u};

    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t length = data.size();
    std::size_t offset = 0;
    for (; offset + kBlockBytes <= length; offset += kBlockBytes) {
        compress(state, bytes + offset);
    }

    // The tail: remaining bytes, a 0x80 terminator, zero padding, and the bit
    // length as a big-endian 64-bit integer. Two blocks cover every case.
    std::array<unsigned char, kBlockBytes * 2> tail{};
    const std::size_t remaining = length - offset;
    if (remaining > 0) {
        std::memcpy(tail.data(), bytes + offset, remaining);
    }
    tail[remaining] = 0x80;
    const std::size_t tail_blocks = remaining + 1 + 8 > kBlockBytes ? 2 : 1;
    const std::uint64_t bits = static_cast<std::uint64_t>(length) * 8;
    const std::size_t bit_offset = tail_blocks * kBlockBytes - 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[bit_offset + i] = static_cast<unsigned char>(bits >> ((7 - i) * 8));
    }
    for (std::size_t block = 0; block < tail_blocks; ++block) {
        compress(state, tail.data() + block * kBlockBytes);
    }

    std::string digest(kMigrationTagBytes, '\0');
    for (std::size_t i = 0; i < state.size(); ++i) {
        digest[i * 4] = static_cast<char>((state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<char>((state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<char>((state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<char>(state[i] & 0xFF);
    }
    return digest;
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    std::string block = key.size() > kBlockBytes ? sha256(key) : key;
    block.resize(kBlockBytes, '\0');

    std::string inner(kBlockBytes, '\0');
    std::string outer(kBlockBytes, '\0');
    for (std::size_t i = 0; i < kBlockBytes; ++i) {
        inner[i] = static_cast<char>(static_cast<unsigned char>(block[i]) ^ 0x36);
        outer[i] = static_cast<char>(static_cast<unsigned char>(block[i]) ^ 0x5c);
    }
    return sha256(outer + sha256(inner + data));
}

bool constantTimeEquals(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        difference |= static_cast<unsigned char>(lhs[i]) ^
                      static_cast<unsigned char>(rhs[i]);
    }
    return difference == 0;
}

}  // namespace cluster
