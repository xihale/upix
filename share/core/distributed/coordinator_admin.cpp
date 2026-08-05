#include "coordinator.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include <cstring>
#include <fstream>

namespace crossscale {

void Coordinator::send_model_list(WorkerConnection& conn) {
    auto models = model_manager_.available_models();

    size_t payload_size = sizeof(ModelListMessage) + models.size() * sizeof(ModelInfoMessage);
    std::vector<uint8_t> payload(payload_size);

    ModelListMessage* msg = reinterpret_cast<ModelListMessage*>(payload.data());
    msg->model_count = static_cast<uint32_t>(models.size());

    size_t offset = sizeof(ModelListMessage);
    for (const auto& model : models) {
        ModelInfoMessage* info = reinterpret_cast<ModelInfoMessage*>(payload.data() + offset);
        std::strncpy(info->name, model.name.c_str(), sizeof(info->name) - 1);
        info->param_size = static_cast<uint32_t>(std::filesystem::file_size(model.param_path));
        info->bin_size = static_cast<uint32_t>(std::filesystem::file_size(model.bin_path));
        std::strncpy(info->hash, model.hash.c_str(), sizeof(info->hash) - 1);
        offset += sizeof(ModelInfoMessage);
    }

    // Sent through the worker's encrypted session (activated by run_handshake).
    (void)send_encrypted(conn, MessageType::MSG_MODEL_LIST, payload.data(), payload_size);
}

void Coordinator::set_models_directory(const std::filesystem::path& dir) {
    model_manager_.set_models_directory(dir);
    (void)model_manager_.scan_models();}

auto Coordinator::handle_model_request(WorkerConnection& conn,
                                        const std::vector<uint8_t>& payload)
    -> expected<void, CoordinatorError> {

    // Helper: always send a ModelDataMessage frame so the worker's
    // download_model() loop stays byte-aligned. A zero data_length signals EOF
    // (or error) and breaks the loop on the worker side. Failing to reply
    // would desync the protocol — the worker blocks waiting for a header and
    // later reads unrelated bytes as a header ("Invalid message header").
    auto send_model_data = [&](const std::string& model_name,
                                uint8_t file_type, uint64_t offset,
                                const uint8_t* data, size_t data_length)
        -> expected<void, CoordinatorError> {
        size_t response_size = sizeof(ModelDataMessage) + data_length;
        std::vector<uint8_t> response(response_size);

        ModelDataMessage* resp_msg = reinterpret_cast<ModelDataMessage*>(response.data());
        std::memset(resp_msg, 0, sizeof(ModelDataMessage));
        std::strncpy(resp_msg->model_name, model_name.c_str(), sizeof(resp_msg->model_name) - 1);
        resp_msg->file_type = file_type;
        resp_msg->file_offset = offset;
        resp_msg->data_length = static_cast<uint32_t>(data_length);

        if (data_length > 0 && data) {
            std::memcpy(response.data() + sizeof(ModelDataMessage), data, data_length);
        }

        // Model data is streamed over the worker's encrypted session.
        if (!send_encrypted(conn, MessageType::MSG_MODEL_DATA, response.data(), response_size)) {
            return std::unexpected(CoordinatorError{16, "Failed to send model data"});
        }
        return {};
    };

    if (payload.size() < sizeof(ModelDataMessage)) {
        // Can't parse the request, but still reply with an empty frame to keep
        // the worker from desyncing. Use empty model name / type 0.
        auto sent = send_model_data("", 0, 0, nullptr, 0);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());        }
        return std::unexpected(CoordinatorError{11, "Invalid model request payload"});
    }

    const ModelDataMessage* request = reinterpret_cast<const ModelDataMessage*>(payload.data());
    std::string model_name(request->model_name);
    uint8_t file_type = request->file_type;
    uint64_t offset = request->file_offset;

    auto model = model_manager_.get_model(model_name);
    if (!model) {
        // Reply with EOF so the worker breaks its download loop cleanly.
        (void)send_model_data(model_name, file_type, offset, nullptr, 0);
        return std::unexpected(CoordinatorError{12, "Model not found: " + model_name});
    }

    std::filesystem::path file_path = (file_type == 0) ? model->param_path : model->bin_path;
    if (!std::filesystem::exists(file_path)) {
        (void)send_model_data(model_name, file_type, offset, nullptr, 0);
        return std::unexpected(CoordinatorError{13, "Model file not found: " + file_path.string()});
    }

    const size_t CHUNK_SIZE = 65536;
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        (void)send_model_data(model_name, file_type, offset, nullptr, 0);
        return std::unexpected(CoordinatorError{14, "Cannot open model file: " + file_path.string()});
    }

    file.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> buffer(CHUNK_SIZE);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    size_t bytes_read = file.gcount();

    if (bytes_read == 0) {
        // Clean EOF: send an empty frame (data_length=0).
        auto sent = send_model_data(model_name, file_type, offset, nullptr, 0);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());        }
        return {};
    }

    auto sent = send_model_data(model_name, file_type, offset, buffer.data(), bytes_read);
    if (!sent.has_value()) {
        return std::unexpected(sent.error());    }

    LOG_INFO("Coordinator", "Sent model data chunk for " + model_name +
             " offset=" + std::to_string(offset) + " size=" + std::to_string(bytes_read));

    return {};
}

} // namespace crossscale
