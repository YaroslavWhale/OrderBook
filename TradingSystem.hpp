#pragma once
#include "types.hpp"
#include <string>
#include <cstdint>

Multipliers load_multiplier(const std::string& symbol);
SnapshotData get_snapshot(const std::string& symbol,
                          uint64_t price_mult,
                          uint64_t volume_mult,
                          int limit = 1000);
void run(std::string symbol, uint64_t depth);
