#include "worker.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include <thread>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace crossscale {

Worker::Worker() = default;

Worker::~Worker() {
    disconnect();
}

auto Worker::connect(const std::string& host, uint16_t port, const std::string& auth_key)
    -> expected<void, WorkerError> {
    
    socket_ = std::make_unique<TCPSocket>();
    
    auto result = socket_->connect(host, port);
    if (!result.has_value()) {
        return WorkerError{1, "Connection failed: " + result.error().message};
    }
    
    auth_key_ = auth_key;
    running_ = true;
    
    AuthRequest auth_req;
    std::strncpy(auth_req.auth_key, auth_key.c_str(), sizeof(auth_req.auth_key) - 1);
    
    LOG_INFO("Worker", "Connected to " + host + ":" + std::to_string(port));
    return {};
}

void Worker::disconnect() {
    running_ = false;
    stop_heartbeat();
    
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
}

bool Worker::is_connected() const {
    return socket_ && socket_->is_connected() && running_;
}

auto Worker::send_profile(const DeviceProfile& profile) -> expected<void, WorkerError> {
    if (!is_connected()) {
        return WorkerError{2, "Not connected"};
    }
    
    DeviceProfileMessage msg;
    std::strncpy(msg.device_id, profile.device_id.c_str(), sizeof(msg.device_id) - 1);
    std::strncpy(msg.device_name, profile.device_name.c_str(), sizeof(msg.device_name) - 1);
    msg.device_type = static_cast<uint8_t>(profile.type);
    msg.tiles_per_second = profile.tiles_per_second;
    msg.recommended_tile_size = profile.recommended_tile_size;
    msg.max_batch_size = profile.max_batch_size;
    std::strncpy(msg.ip_address, profile.ip_address.c_str(), sizeof(msg.ip_address) - 1);
    msg.port = profile.port;
    
    MessageHeader header;
    header.type = MessageType::MSG_DEVICE_PROFILE;
    header.payload_length = sizeof(msg);
    
    auto data = Protocol::serialize(header, &msg);
    auto result = socket_->send_all(data.data(), data.size());
    
    if (!result.has_value()) {
        return WorkerError{3, "Failed to send profile"};
    }
    
    return {};
}

auto Worker::receive_task() -> expected<Task, WorkerError> {
    if (!is_connected()) {
        return WorkerError{2, "Not connected"};
    }
    
    std::vector<uint8_t> header_buffer(Protocol::HEADER_SIZE);
    auto result = socket_->receive_all(header_buffer.data(), header_buffer.size());
    
    if (!result.has_value()) {
        return WorkerError{4, "Failed to receive task"};
    }
    
    auto header = Protocol::parse_header(header_buffer.data(), header_buffer.size());
    if (!header) {
        return WorkerError{5, "Invalid message header"};
    }
    
    if (header->type == MessageType::MSG_TASK_ASSIGNMENT) {
        std::vector<uint8_t> payload(header->payload_length);
        result = socket_->receive_all(payload.data(), payload.size());
        
        if (!result.has_value()) {
            return WorkerError{4, "Failed to receive task payload"};
        }
        
        TaskAssignmentMessage* msg = reinterpret_cast<TaskAssignmentMessage*>(payload.data());
        
        Task task;
        task.task_id = msg->task_id;
        task.model_name = msg->model_name;
        
        size_t tile_info_offset = sizeof(TaskAssignmentMessage);
        for (uint32_t i = 0; i < msg->tile_count; ++i) {
            TileInfo* tile = reinterpret_cast<TileInfo*>(payload.data() + tile_info_offset + i * sizeof(TileInfo));
            task.tile_indices.push_back(tile->x);
        }
        
        if (on_task_received_) {
            on_task_received_(task);
        }
        
        return task;
    }
    
    return WorkerError{6, "Unexpected message type"};
}

auto Worker::execute_task(const Task& task, Engine& engine,
                            std::function<void(float)> progress_callback)
    -> expected<std::vector<uint8_t>, WorkerError> {
    
    if (!is_connected()) {
        return WorkerError{2, "Not connected"};
    }
    
    MessageHeader accept_header;
    accept_header.type = MessageType::MSG_TASK_ACCEPT;
    accept_header.payload_length = static_cast<uint32_t>(task.task_id.size() + 1);
    auto accept_data = Protocol::serialize(accept_header, task.task_id.c_str());
    auto result = socket_->send_all(accept_data.data(), accept_data.size());
    if (!result.has_value()) {
        return WorkerError{8, "Failed to send task acceptance"};
    }
    
    if (engine.model_name() != task.model_name) {
        std::filesystem::path models_dir = "./models";
        std::string param_path = (models_dir / (task.model_name + ".param")).string();
        std::string bin_path = (models_dir / (task.model_name + ".bin")).string();

        auto load_result = engine.load_model(param_path, bin_path);
        if (!load_result.has_value()) {
            return WorkerError{9, "Failed to load model: " + task.model_name +
                                  " - " + load_result.error().message};
        }
    }
    
    int scale_factor = engine.get_scale_factor();
    std::vector<ProcessedTile> processed_tiles;
    processed_tiles.reserve(task.tile_indices.size());
    
    for (size_t i = 0; i < task.tile_indices.size(); ++i) {
        if (!running_) {
            return WorkerError{7, "Task cancelled"};
        }
        
        std::vector<uint8_t> tile_data;
        uint32_t tile_x = 0, tile_y = 0, tile_width = 0, tile_height = 0;
        uint8_t channels = 3;
        
        auto tile_result = receive_tile_data(tile_x, tile_y, tile_width, tile_height, 
                                              channels, tile_data);
        if (!tile_result.has_value()) {
            return tile_result.error();
        }
        
        LOG_INFO("Worker", "Processing tile " + std::to_string(i + 1) + "/" + 
                 std::to_string(task.tile_indices.size()) + " [" + std::to_string(tile_width) + 
                 "x" + std::to_string(tile_height) + "]");
        
        std::vector<float> input_float(tile_data.size());
        for (size_t j = 0; j < tile_data.size(); ++j) {
            input_float[j] = tile_data[j] / 255.0f;
        }
        
        int out_width = tile_width * scale_factor;
        int out_height = tile_height * scale_factor;
        std::vector<float> output_float(out_width * out_height * channels);
        
        if (!engine.process(input_float.data(), output_float.data(), 
                           tile_width, tile_height, channels)) {
            return WorkerError{10, "Engine processing failed for tile " + std::to_string(i)};
        }
        
        std::vector<uint8_t> output_data(output_float.size());
        for (size_t j = 0; j < output_float.size(); ++j) {
            float val = output_float[j] * 255.0f;
            output_data[j] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
        }
        
        ProcessedTile pt;
        pt.tile_x = tile_x;
        pt.tile_y = tile_y;
        pt.original_width = tile_width;
        pt.original_height = tile_height;
        pt.output_width = out_width;
        pt.output_height = out_height;
        pt.channels = channels;
        pt.data = std::move(output_data);
        processed_tiles.push_back(std::move(pt));
        
        float progress = static_cast<float>(i + 1) / task.tile_indices.size();
        if (progress_callback) {
            progress_callback(progress);
        }
        
        send_progress_update(task.task_id, progress);
    }
    
    return serialize_processed_tiles(processed_tiles);
}

auto Worker::receive_tile_data(uint32_t& tile_x, uint32_t& tile_y,
                                uint32_t& width, uint32_t& height,
                                uint8_t& channels, std::vector<uint8_t>& data)
    -> expected<void, WorkerError> {

    std::vector<uint8_t> header_buffer(Protocol::HEADER_SIZE);
    auto result = socket_->receive_all(header_buffer.data(), header_buffer.size());
    if (!result.has_value()) {
        return WorkerError{4, "Failed to receive tile header"};
    }

    auto header = Protocol::parse_header(header_buffer.data(), header_buffer.size());
    if (!header) {
        return WorkerError{5, "Invalid tile message header"};
    }

    if (header->type != MessageType::MSG_IMAGE_TILE) {
        return WorkerError{6, "Expected IMAGE_TILE message, got different type"};
    }

    if (header->payload_length < sizeof(ImageTileMessage)) {
        return WorkerError{5, "Invalid tile message payload size"};
    }

    std::vector<uint8_t> payload(header->payload_length);
    result = socket_->receive_all(payload.data(), payload.size());
    if (!result.has_value()) {
        return WorkerError{4, "Failed to receive tile payload"};
    }

    ImageTileMessage* tile_msg = reinterpret_cast<ImageTileMessage*>(payload.data());
    tile_x = tile_msg->tile_x;
    tile_y = tile_msg->tile_y;
    width = tile_msg->width;
    height = tile_msg->height;
    channels = tile_msg->channels;

    size_t data_offset = sizeof(ImageTileMessage);
    size_t data_size = tile_msg->data_length;

    if (payload.size() < data_offset + data_size) {
        return WorkerError{5, "Tile data size mismatch"};
    }

    data.resize(data_size);
    std::memcpy(data.data(), payload.data() + data_offset, data_size);

    return {};
}

void Worker::send_progress_update(const std::string& task_id, float progress) {
    if (!socket_ || !socket_->is_connected()) {
        return;
    }

    struct ProgressMessage {
        char task_id[36];
        float progress;
    };

    ProgressMessage msg;
    std::strncpy(msg.task_id, task_id.c_str(), sizeof(msg.task_id) - 1);
    msg.progress = progress;

    MessageHeader header;
    header.type = MessageType::MSG_TASK_PROGRESS;
    header.payload_length = sizeof(msg);

    auto data = Protocol::serialize(header, &msg);
    socket_->send_all(data.data(), data.size());
}

auto Worker::serialize_processed_tiles(const std::vector<ProcessedTile>& tiles)
    -> std::vector<uint8_t> {

    size_t total_size = sizeof(uint32_t);
    for (const auto& tile : tiles) {
        total_size += sizeof(uint32_t) * 6;
        total_size += sizeof(uint8_t);
        total_size += sizeof(uint32_t);
        total_size += tile.data.size();
    }

    std::vector<uint8_t> result;
    result.reserve(total_size);

    uint32_t tile_count = static_cast<uint32_t>(tiles.size());
    result.insert(result.end(),
                  reinterpret_cast<uint8_t*>(&tile_count),
                  reinterpret_cast<uint8_t*>(&tile_count) + sizeof(tile_count));

    for (const auto& tile : tiles) {
        uint32_t x = tile.tile_x;
        uint32_t y = tile.tile_y;
        uint32_t orig_w = tile.original_width;
        uint32_t orig_h = tile.original_height;
        uint32_t out_w = tile.output_width;
        uint32_t out_h = tile.output_height;
        uint8_t ch = tile.channels;
        uint32_t data_size = static_cast<uint32_t>(tile.data.size());

        result.insert(result.end(), reinterpret_cast<uint8_t*>(&x),
                      reinterpret_cast<uint8_t*>(&x) + sizeof(x));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&y),
                      reinterpret_cast<uint8_t*>(&y) + sizeof(y));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&orig_w),
                      reinterpret_cast<uint8_t*>(&orig_w) + sizeof(orig_w));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&orig_h),
                      reinterpret_cast<uint8_t*>(&orig_h) + sizeof(orig_h));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&out_w),
                      reinterpret_cast<uint8_t*>(&out_w) + sizeof(out_w));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&out_h),
                      reinterpret_cast<uint8_t*>(&out_h) + sizeof(out_h));
        result.insert(result.end(), &ch, &ch + 1);
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&data_size),
                      reinterpret_cast<uint8_t*>(&data_size) + sizeof(data_size));
        result.insert(result.end(), tile.data.begin(), tile.data.end());
    }

    return result;
}

auto Worker::submit_results(const std::vector<uint8_t>& results)
    -> expected<void, WorkerError> {
    
    if (!is_connected()) {
        return WorkerError{2, "Not connected"};
    }
    
    MessageHeader header;
    header.type = MessageType::MSG_TASK_RESULT;
    header.payload_length = static_cast<uint32_t>(results.size());
    
    auto data = Protocol::serialize(header, results.data());
    auto result = socket_->send_all(data.data(), data.size());
    
    if (!result.has_value()) {
        return WorkerError{8, "Failed to submit results"};
    }
    
    return {};
}

void Worker::start_heartbeat(std::chrono::seconds interval) {
    heartbeat_running_ = true;
    std::thread(&Worker::heartbeat_loop, this, interval).detach();
}

void Worker::stop_heartbeat() {
    heartbeat_running_ = false;
}

void Worker::heartbeat_loop(std::chrono::seconds interval) {
    while (heartbeat_running_ && running_) {
        if (socket_ && socket_->is_connected()) {
            MessageHeader header;
            header.type = MessageType::MSG_HEARTBEAT;
            header.payload_length = 0;
            
            auto data = Protocol::serialize(header, nullptr);
            socket_->send_all(data.data(), data.size());
        }
        
        std::this_thread::sleep_for(interval);
    }
}

void Worker::set_models_directory(const std::filesystem::path& dir) {
    model_manager_.set_models_directory(dir);
}

auto Worker::sync_models() -> expected<void, WorkerError> {
    if (!is_connected()) {
        return WorkerError{2, "Not connected"};
    }

    LOG_INFO("Worker", "Waiting for model list from coordinator...");

    std::vector<uint8_t> header_buffer(Protocol::HEADER_SIZE);
    auto result = socket_->receive_all(header_buffer.data(), header_buffer.size());
    if (!result.has_value()) {
        return WorkerError{11, "Failed to receive model list header"};
    }

    auto header = Protocol::parse_header(header_buffer.data(), header_buffer.size());
    if (!header) {
        return WorkerError{12, "Invalid model list header"};
    }

    if (header->type != MessageType::MSG_MODEL_LIST) {
        return WorkerError{13, "Expected MODEL_LIST message"};
    }

    std::vector<uint8_t> payload(header->payload_length);
    result = socket_->receive_all(payload.data(), payload.size());
    if (!result.has_value()) {
        return WorkerError{14, "Failed to receive model list payload"};
    }

    ModelListMessage* msg = reinterpret_cast<ModelListMessage*>(payload.data());
    uint32_t model_count = msg->model_count;

    LOG_INFO("Worker", "Received model list with " + std::to_string(model_count) + " models");

    size_t offset = sizeof(ModelListMessage);
    for (uint32_t i = 0; i < model_count; ++i) {
        if (offset + sizeof(ModelInfoMessage) > payload.size()) {
            break;
        }

        ModelInfoMessage* info = reinterpret_cast<ModelInfoMessage*>(payload.data() + offset);
        std::string model_name(info->name);
        std::string model_hash(info->hash);

        offset += sizeof(ModelInfoMessage);

        if (model_manager_.has_model(model_name)) {
            auto local_model = model_manager_.get_model(model_name);
            if (local_model && local_model->hash == model_hash) {
                LOG_INFO("Worker", "Model " + model_name + " is up to date");
                continue;
            }
        }

        LOG_INFO("Worker", "Requesting model: " + model_name);

        auto download_result = download_model(model_name, 0);
        if (!download_result.has_value()) {
            LOG_WARNING("Worker", "Failed to download model " + model_name);
        }

        download_result = download_model(model_name, 1);
        if (!download_result.has_value()) {
            LOG_WARNING("Worker", "Failed to download model bin " + model_name);
        }
    }

    model_manager_.scan_models();
    LOG_INFO("Worker", "Model sync complete");

    return {};
}

auto Worker::download_model(const std::string& model_name, uint8_t file_type)
    -> expected<void, WorkerError> {

    uint64_t offset = 0;
    std::vector<uint8_t> file_data;

    std::filesystem::path temp_path = model_manager_.models_directory() / 
                                       (model_name + (file_type == 0 ? ".param" : ".bin") + ".tmp");

    while (true) {
        ModelDataMessage request;
        std::strncpy(request.model_name, model_name.c_str(), sizeof(request.model_name) - 1);
        request.file_type = file_type;
        request.file_offset = offset;
        request.data_length = 0;

        MessageHeader req_header;
        req_header.type = MessageType::MSG_MODEL_REQUEST;
        req_header.payload_length = sizeof(request);

        auto req_data = Protocol::serialize(req_header, &request);
        auto send_result = socket_->send_all(req_data.data(), req_data.size());
        if (!send_result.has_value()) {
            return WorkerError{15, "Failed to send model request"};
        }

        std::vector<uint8_t> resp_header_buffer(Protocol::HEADER_SIZE);
        auto recv_result = socket_->receive_all(resp_header_buffer.data(), resp_header_buffer.size());
        if (!recv_result.has_value()) {
            return WorkerError{16, "Failed to receive model data header"};
        }

        auto resp_header = Protocol::parse_header(resp_header_buffer.data(), resp_header_buffer.size());
        if (!resp_header) {
            return WorkerError{17, "Invalid model data header"};
        }

        if (resp_header->type != MessageType::MSG_MODEL_DATA) {
            return WorkerError{18, "Expected MODEL_DATA message"};
        }

        std::vector<uint8_t> resp_payload(resp_header->payload_length);
        recv_result = socket_->receive_all(resp_payload.data(), resp_payload.size());
        if (!recv_result.has_value()) {
            return WorkerError{19, "Failed to receive model data payload"};
        }

        const ModelDataMessage* data_msg = reinterpret_cast<const ModelDataMessage*>(resp_payload.data());
        uint32_t data_length = data_msg->data_length;

        if (data_length == 0) {
            break;
        }

        size_t data_offset = sizeof(ModelDataMessage);
        file_data.insert(file_data.end(), 
                         resp_payload.data() + data_offset,
                         resp_payload.data() + data_offset + data_length);

        offset += data_length;

        if (data_length < 65536) {
            break;
        }
    }

    std::ofstream file(temp_path, std::ios::binary);
    if (!file) {
        return WorkerError{20, "Cannot create temp file"};
    }

    file.write(reinterpret_cast<const char*>(file_data.data()), file_data.size());
    file.close();

    std::filesystem::path final_path = model_manager_.models_directory() / 
                                        (model_name + (file_type == 0 ? ".param" : ".bin"));
    std::filesystem::rename(temp_path, final_path);

    LOG_INFO("Worker", "Downloaded " + model_name + (file_type == 0 ? ".param" : ".bin") + 
             " (" + std::to_string(file_data.size()) + " bytes)");

    return {};
}

} // namespace crossscale
