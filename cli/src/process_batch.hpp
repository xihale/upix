// Directory batch path — now a thin adapter over share's batch core
// (docs/SPEC-frontends.md S1: the pipeline lives in share; the desktop drives
// the same upix::run_batch directly, in-process).
//
// This adapter owns only the CLI seams: the signal-driven cancel flag, the
// UPIX_PROGRESS machine/ human encoding, and the exit-code mapping.
#pragma once

#include "args.hpp"
#include "common.hpp"

// Directory batch processing. cmd_process delegates here when the input path
// is a directory. Returns the process exit code (0 success).
int run_batch(const args::Process& a, EffectiveProcessingConfig effective);
