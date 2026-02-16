#include "engine.hpp"

namespace crossscale {

std::unique_ptr<Engine> create_ncnn_engine() {
    extern std::unique_ptr<Engine> create_ncnn_engine_impl();
    return create_ncnn_engine_impl();
}

}
