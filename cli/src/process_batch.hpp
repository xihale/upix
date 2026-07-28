// Directory batch path, split out of cmd_process (Layer 2 Task 4).
//
// run_batch() is the verbatim move of the former `if (is_dir) { ... }` block
// out of cmd_process(). cmd_process is a free function at global scope, so
// run_batch is declared at global scope too (no namespace).
#pragma once

#include "args.hpp"

// Directory batch processing. cmd_process delegates here when the input path
// is a directory. Returns the process exit code (0 success).
int run_batch(const args::Process& a);
