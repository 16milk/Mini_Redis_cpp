#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cluster {

// Migration proofs are assertions one shard makes about its own Raft log, and
// the shard verifying them cannot read that log. Signing them with a key that
// only cluster members hold keeps the assertion from being manufactured by a
// client, a stale tool, or anything else that can reach the admin port.
//
// The scope is deliberately narrow: this authenticates "some cluster member
// said this", not "the source shard's quorum agreed to this". A compromised
// member still holds the key. Closing that gap needs per-replica signatures
// over the fence entry, which this protocol leaves for later.
constexpr std::size_t kMigrationTagBytes = 32;

std::string sha256(const std::string& data);

// HMAC-SHA256, truncated to nothing: the full 32-byte tag.
std::string hmacSha256(const std::string& key, const std::string& data);

// Constant-time compare, so a verifier cannot be turned into a tag oracle by
// timing how far a forged prefix matched.
bool constantTimeEquals(const std::string& lhs, const std::string& rhs);

// The key every shard uses to sign and check migration proofs. It is cluster
// configuration, not per-node state, so it never enters a Raft log or a
// snapshot; replicas are handed the same bytes at construction time.
class MigrationKey {
public:
    MigrationKey() = default;
    explicit MigrationKey(std::string secret) : secret_(std::move(secret)) {}

    bool configured() const { return !secret_.empty(); }
    std::string sign(const std::string& body) const {
        return hmacSha256(secret_, body);
    }
    bool verify(const std::string& body, const std::string& tag) const {
        return constantTimeEquals(sign(body), tag);
    }

private:
    std::string secret_;
};

}  // namespace cluster
