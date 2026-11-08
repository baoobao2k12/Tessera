// UCI protocol front end.
#pragma once

#include <cstdint>
#include <string>

namespace UCI {
void loop(int argc, char* argv[]);
// Runs the reproducible benchmark; returns total nodes.
uint64_t bench(int depth, int threads, int hashMb, bool verbose);
}  // namespace UCI
