#include <cstdio>
#include <iostream>

#include "bitboard.h"
#include "eval.h"
#include "platform/cpu_check.h"
#include "position.h"
#include "search.h"
#include "tt.h"
#include "uci.h"

int main(int argc, char* argv[]) {
    // Must run before any code that may use POPCNT/BMI2 instructions.
    if (const char* msg = cpu_check_message()) {
        std::fprintf(stderr, "Tessera: %s\n", msg);
        return 1;
    }

    Bitboards::init();
    Position::init();
    Eval::init();
    Search::init();
    TT.resize(16);
    Threads.set(1);

    UCI::loop(argc, argv);

    Threads.set(0);
    return 0;
}
