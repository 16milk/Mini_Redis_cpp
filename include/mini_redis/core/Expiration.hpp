#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

using UnixMillis = std::int64_t;
using ExpireMap = std::unordered_map<std::string, UnixMillis>;
