#pragma once
// Returns nullptr when the running CPU supports the compiled instruction set,
// otherwise a human readable explanation.
const char* cpu_check_message();
const char* build_arch_name();
