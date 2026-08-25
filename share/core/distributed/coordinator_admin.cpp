#include "coordinator.hpp"

namespace upix {

// The coordinator uses the models directory to resolve model files for its OWN
// local engine (process_locally). In the 1:1 model the phone submits images and
// the PC upscales them here — models are no longer pushed to worker nodes.
void Coordinator::set_models_directory(const std::filesystem::path& dir) {
    model_manager_.set_models_directory(dir);
    (void)model_manager_.scan_models();
}

} // namespace upix
