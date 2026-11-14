// Per-instruction replacements for Super Mario Odyssey (port of tools/odyssey_patches.py).
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "gen.h"

namespace ngen {

// build id -> patches
const std::map<std::string, std::map<uint64_t, PatchFn>> &patches_by_build();
// build id -> (address -> hle symbol name)
const std::map<std::string, std::map<uint64_t, std::string>> &hle_by_build();

}  // namespace ngen
