// Function discovery: .eh_frame_hdr binary search table (port of tools/analyze.py).
#pragma once

#include <cstdint>
#include <vector>

#include "nso.h"

namespace ngen {

// Function start addresses from .eh_frame_hdr's binary search table.
std::vector<uint64_t> eh_frame_functions(const Nso &n);

}  // namespace ngen
