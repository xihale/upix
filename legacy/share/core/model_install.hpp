// Local model import: copy user .param/.bin pairs into the models dir with
// staged backups and rollback (formerly legacy/cli/src/cmd_model.cpp's
// cmd_model_import body). Failure of a pair must never leave a .param
// without its matching .bin installed.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace upix {

struct ImportOutcome {
    bool ok = false;
    // Human-readable first error when !ok (empty otherwise).
    std::string error;
    // Stems successfully installed (order follows the input files).
    std::vector<std::string> imported;
    // Non-fatal conversion warnings (probe_model_sanity): the install
    // stands, the caller decides how loudly to show them.
    std::vector<std::string> warnings;
};

// Import local .param/.bin files: pairs matched by stem, each pair staged
// beside its destination, previous files backed up, renames rolled back on
// any failure. `models_dir` is created when missing.
auto import_model_files(const std::vector<std::string>& files,
                        const std::filesystem::path& models_dir)
    -> ImportOutcome;

}  // namespace upix
