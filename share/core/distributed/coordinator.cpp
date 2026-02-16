#include "coordinator.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include <thread>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <fstream>

namespace crossscale {

Coordinator::Coordinator() = default;

Coordinator::~Coordinator() {
    stop();
}

auto Coordinator::start(uint16_t port, const std::string& auth_key)
    -> expected<void, CoordinatorError> {
    
    if (running_) {
        return {};
    }
    
    server_ = std::make_unique<TCPServer>();
    
    auto result = server_->listen(port);
    if (!result.has_value()) {
        return CoordinatorError{1, "Failed to start server: " + result.error().message};
    }
    
    port_ = port;
    auth_key_ = auth_key.empty() ? network_utils::generate_auth_key() : auth_key;
    server_name_ = "CrossScale-Server";
    running_ = true;
    
    std::thread(&Coordinator::accept_loop, this).detach();
    
    LOG_INFO("Coordinator", "Server started on port " + std::to_string(port_));
    return {};
}

void Coordinator::stop() {
    running_ = false;
    
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.clear();
    }
    
    if (server_) {
        server_->stop();
        server_.reset();
    }
}

bool Coordinator::is_running() const {
    return running_;
}

auto Coordinator::generate_connection_info() -> ConnectionInfo {
    ConnectionInfo info;
    
    auto addresses = network_utils::get_local_addresses();
    info.ip = addresses.empty() ? "127.0.0.1" : addresses[0];
    info.port = port_;
    info.auth_key = auth_key_;
    info.server_name = server_name_;
    
    return info;
}

auto Coordinator::get_connected_devices() -> std::vector<DeviceProfile> {
    return scheduler_.get_all_devices();
}

auto Coordinator::device_count() const -> size_t {
    return scheduler_.device_count();
}

auto Coordinator::distribute_task(const std::vector<uint8_t>& image_data,
                                   int width, int height, int channels,
                                   const std::string& model_name,
                                   int scale_factor)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    std::lock_guard<std::mutex> lock(workers_mutex_);

    if (workers_.empty()) {
        return CoordinatorError{2, "No workers connected"};
    }

    const int tile_size = 256;
    const int overlap = 16;

    int tiles_x = (width + tile_size - 1) / tile_size;
    int tiles_y = (height + tile_size - 1) / tile_size;
    int total_tiles = tiles_x * tiles_y;

    LOG_INFO("Coordinator", "Splitting image into " + std::to_string(total_tiles) +
             " tiles (" + std::to_string(tiles_x) + "x" + std::to_string(tiles_y) + ")");

    uint32_t total_tiles_u32 = static_cast<uint32_t>(total_tiles);
    auto assignments = scheduler_.assign_tiles(total_tiles_u32, model_name);

    if (assignments.empty()) {
        return CoordinatorError{3, "Failed to assign tasks"};
    }

    task_cancelled_ = false;
    active_tasks_.clear();
    pending_results_.clear();

    std::string task_id = crypto::generate_uuid();

    for (const auto& assignment : assignments) {
        auto it = workers_.find(assignment.device_id);
        if (it == workers_.end()) continue;

        TaskAssignmentMessage msg;
        std::strncpy(msg.task_id, task_id.c_str(), sizeof(msg.task_id) - 1);
        std::strncpy(msg.model_name, model_name.c_str(), sizeof(msg.model_name) - 1);
        msg.tile_count = static_cast<uint32_t>(assignment.assigned_tiles.size());

        MessageHeader header;
        header.type = MessageType::MSG_TASK_ASSIGNMENT;
        header.payload_length = sizeof(msg) + msg.tile_count * sizeof(TileInfo);

        std::vector<uint8_t> payload(header.payload_length);
        std::memcpy(payload.data(), &msg, sizeof(msg));

        size_t offset = sizeof(msg);
        for (uint32_t tile_idx : assignment.assigned_tiles) {
            int tile_x = tile_idx % tiles_x;
            int tile_y = tile_idx / tiles_x;

            int x = tile_x * tile_size;
            int y = tile_y * tile_size;
            int w = std::min(tile_size, width - x);
            int h = std::min(tile_size, height - y);

            TileInfo tile;
            tile.x = static_cast<uint32_t>(x);
            tile.y = static_cast<uint32_t>(y);
            tile.width = static_cast<uint32_t>(w);
            tile.height = static_cast<uint32_t>(h);
            tile.overlap = static_cast<uint32_t>(overlap);
            std::memcpy(payload.data() + offset, &tile, sizeof(tile));
            offset += sizeof(tile);
        }

        auto data = Protocol::serialize(header, payload.data());
        auto result = it->second.socket->send_all(data.data(), data.size());
        if (!result.has_value()) {
            LOG_ERROR("Coordinator", "Failed to send task to worker " + assignment.device_id);
            continue;
        }

        for (uint32_t tile_idx : assignment.assigned_tiles) {
            int tile_x = tile_idx % tiles_x;
            int tile_y = tile_idx / tiles_x;
            int x = tile_x * tile_size;
            int y = tile_y * tile_size;
            int w = std::min(tile_size, width - x);
            int h = std::min(tile_size, height - y);

            auto tile_result = send_tile_data(it->second.socket, task_id, tile_idx,
                                               x, y, w, h, channels, image_data, width, height);
            if (!tile_result.has_value()) {
                LOG_ERROR("Coordinator", "Failed to send tile " + std::to_string(tile_idx));
            }
        }

        ActiveTask at;
        at.task_id = task_id;
        at.worker_id = assignment.device_id;
        at.tile_indices = assignment.assigned_tiles;
        at.completed = false;
        active_tasks_[task_id + "_" + assignment.device_id] = std::move(at);
    }

    LOG_INFO("Coordinator", "Waiting for results from " + std::to_string(assignments.size()) + " workers");

    auto results = collect_results(task_id, assignments.size(), total_tiles);
    if (!results.has_value()) {
        return CoordinatorError{4, "Failed to collect results: " + results.error().message};
    }

    LOG_INFO("Coordinator", "Stitching " + std::to_string(results->size()) + " tiles into final image");

    auto stitched = stitch_tiles(*results, width, height, channels, scale_factor);
    if (!stitched.has_value()) {
        return CoordinatorError{5, "Failed to stitch tiles: " + stitched.error().message};
    }

    return stitched;
}

void Coordinator::cancel_current_task() {
    task_cancelled_ = true;
}

void Coordinator::accept_loop() {
    while (running_) {
        auto client = server_->accept();
        if (!client.has_value()) {
            if (running_) {
                LOG_ERROR("Coordinator", "Accept failed: " + client.error().message);
            }
            continue;
        }
        
        std::thread(&Coordinator::handle_client, this, std::move(client.value())).detach();
    }
}

void Coordinator::handle_client(std::unique_ptr<TCPSocket> socket) {
    WorkerConnection conn;
    conn.socket = std::move(socket);
    conn.device_id = crypto::generate_uuid();
    
    std::vector<uint8_t> header_buffer(Protocol::HEADER_SIZE);
    auto result = conn.socket->receive_all(header_buffer.data(), header_buffer.size());
    
    if (!result.has_value()) {
        return;
    }
    
    auto header = Protocol::parse_header(header_buffer.data(), header_buffer.size());
    if (!header) {
        return;
    }
    
    if (header->type == MessageType::MSG_AUTH_REQUEST) {
        std::vector<uint8_t> payload(header->payload_length);
        conn.socket->receive_all(payload.data(), payload.size());
        
        AuthRequest* auth_req = reinterpret_cast<AuthRequest*>(payload.data());
        if (!authenticate(conn, auth_req->auth_key)) {
            AuthResponse resp;
            resp.authenticated = 0;
            std::strncpy(resp.device_id, "", sizeof(resp.device_id) - 1);
            
            MessageHeader resp_header;
            resp_header.type = MessageType::MSG_AUTH_RESPONSE;
            resp_header.payload_length = sizeof(resp);
            
            auto data = Protocol::serialize(resp_header, &resp);
            conn.socket->send_all(data.data(), data.size());
            return;
        }
        
        AuthResponse resp;
        resp.authenticated = 1;
        std::strncpy(resp.device_id, conn.device_id.c_str(), sizeof(resp.device_id) - 1);
        
        MessageHeader resp_header;
        resp_header.type = MessageType::MSG_AUTH_RESPONSE;
        resp_header.payload_length = sizeof(resp);
        
        auto data = Protocol::serialize(resp_header, &resp);
        conn.socket->send_all(data.data(), data.size());
        
        conn.authenticated = true;
        
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_[conn.device_id] = std::move(conn);
        }
        
        LOG_INFO("Coordinator", "Worker connected: " + conn.device_id);
        
        if (on_device_connected_) {
            on_device_connected_(workers_[conn.device_id].profile);
        }
    }
}

bool Coordinator::authenticate(WorkerConnection& conn, const std::string& received_key) {
    return received_key == auth_key_;
}

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

    MessageHeader header;
    header.type = MessageType::MSG_MODEL_LIST;
    header.payload_length = static_cast<uint32_t>(payload_size);

    auto data = Protocol::serialize(header, payload.data());
    conn.socket->send_all(data.data(), data.size());
}

void Coordinator::set_models_directory(const std::filesystem::path& dir) {
    model_manager_.set_models_directory(dir);
    model_manager_.scan_models();
}

auto Coordinator::sync_models_with_worker(const std::string& device_id)
    -> expected<void, CoordinatorError> {

    auto it = workers_.find(device_id);
    if (it == workers_.end()) {
        return CoordinatorError{10, "Worker not found"};
    }

    send_model_list(it->second);
    LOG_INFO("Coordinator", "Sent model list to worker " + device_id);
    return {};
}

auto Coordinator::handle_model_request(WorkerConnection& conn,
                                        const std::vector<uint8_t>& payload)
    -> expected<void, CoordinatorError> {

    if (payload.size() < sizeof(ModelDataMessage)) {
        return CoordinatorError{11, "Invalid model request payload"};
    }

    const ModelDataMessage* request = reinterpret_cast<const ModelDataMessage*>(payload.data());
    std::string model_name(request->model_name);
    uint8_t file_type = request->file_type;
    uint64_t offset = request->file_offset;

    auto model = model_manager_.get_model(model_name);
    if (!model) {
        return CoordinatorError{12, "Model not found: " + model_name};
    }

    std::filesystem::path file_path = (file_type == 0) ? model->param_path : model->bin_path;
    if (!std::filesystem::exists(file_path)) {
        return CoordinatorError{13, "Model file not found"};
    }

    const size_t CHUNK_SIZE = 65536;
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return CoordinatorError{14, "Cannot open model file"};
    }

    file.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> buffer(CHUNK_SIZE);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    size_t bytes_read = file.gcount();

    if (bytes_read <= 0) {
        return CoordinatorError{15, "End of file reached"};
    }

    size_t response_size = sizeof(ModelDataMessage) + bytes_read;
    std::vector<uint8_t> response(response_size);

    ModelDataMessage* resp_msg = reinterpret_cast<ModelDataMessage*>(response.data());
    std::strncpy(resp_msg->model_name, model_name.c_str(), sizeof(resp_msg->model_name) - 1);
    resp_msg->file_type = file_type;
    resp_msg->file_offset = offset;
    resp_msg->data_length = static_cast<uint32_t>(bytes_read);

    std::memcpy(response.data() + sizeof(ModelDataMessage), buffer.data(), bytes_read);

    MessageHeader header;
    header.type = MessageType::MSG_MODEL_DATA;
    header.payload_length = static_cast<uint32_t>(response_size);

    auto data = Protocol::serialize(header, response.data());
    auto result = conn.socket->send_all(data.data(), data.size());

    if (!result.has_value()) {
        return CoordinatorError{16, "Failed to send model data"};
    }

    LOG_INFO("Coordinator", "Sent model data chunk for " + model_name +
             " offset=" + std::to_string(offset) + " size=" + std::to_string(bytes_read));

    return {};
}

auto Coordinator::send_tile_data(const std::unique_ptr<TCPSocket>& socket,
                                  const std::string& task_id,
                                  uint32_t tile_index,
                                  int x, int y, int width, int height,
                                  int channels,
                                  const std::vector<uint8_t>& image_data,
                                  int img_width, int img_height)
    -> expected<void, CoordinatorError> {

    size_t tile_data_size = width * height * channels;
    std::vector<uint8_t> tile_data;
    tile_data.resize(tile_data_size);

    for (int row = 0; row < height; ++row) {
        int src_y = y + row;
        int src_row_offset = (src_y * img_width + x) * channels;
        int dst_row_offset = row * width * channels;
        std::memcpy(tile_data.data() + dst_row_offset,
                    image_data.data() + src_row_offset,
                    width * channels);
    }

    size_t payload_size = sizeof(ImageTileMessage) + tile_data_size;
    std::vector<uint8_t> payload(payload_size);

    ImageTileMessage* msg = reinterpret_cast<ImageTileMessage*>(payload.data());
    std::strncpy(msg->task_id, task_id.c_str(), sizeof(msg->task_id) - 1);
    msg->tile_index = tile_index;
    msg->tile_x = static_cast<uint32_t>(x);
    msg->tile_y = static_cast<uint32_t>(y);
    msg->width = static_cast<uint32_t>(width);
    msg->height = static_cast<uint32_t>(height);
    msg->channels = static_cast<uint8_t>(channels);
    msg->pixel_format = 0;
    msg->data_length = static_cast<uint32_t>(tile_data_size);

    std::memcpy(payload.data() + sizeof(ImageTileMessage), tile_data.data(), tile_data_size);

    MessageHeader header;
    header.type = MessageType::MSG_IMAGE_TILE;
    header.payload_length = static_cast<uint32_t>(payload_size);

    auto data = Protocol::serialize(header, payload.data());
    auto result = socket->send_all(data.data(), data.size());

    if (!result.has_value()) {
        return CoordinatorError{6, "Failed to send tile data"};
    }

    return {};
}

auto Coordinator::collect_results(const std::string& task_id,
                                   size_t num_workers,
                                   int total_tiles)
    -> expected<std::vector<ProcessedTile>, CoordinatorError> {

    std::vector<ProcessedTile> all_results;
    all_results.reserve(total_tiles);

    std::chrono::seconds timeout(300);
    auto start_time = std::chrono::steady_clock::now();

    size_t completed_workers = 0;

    while (completed_workers < num_workers && !task_cancelled_) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            return CoordinatorError{7, "Task timeout"};
        }

        for (auto& [key, active_task] : active_tasks_) {
            if (active_task.completed || active_task.task_id != task_id) {
                continue;
            }

            auto worker_it = workers_.find(active_task.worker_id);
            if (worker_it == workers_.end()) {
                active_task.completed = true;
                completed_workers++;
                continue;
            }

            std::vector<uint8_t> header_buffer(Protocol::HEADER_SIZE);
            worker_it->second.socket->set_timeout(100);
            auto result = worker_it->second.socket->receive_all(header_buffer.data(),
                                                                 header_buffer.size());

            if (!result.has_value()) {
                continue;
            }

            auto header = Protocol::parse_header(header_buffer.data(), header_buffer.size());
            if (!header) {
                continue;
            }

            if (header->type == MessageType::MSG_TASK_RESULT) {
                std::vector<uint8_t> payload(header->payload_length);
                result = worker_it->second.socket->receive_all(payload.data(), payload.size());
                if (!result.has_value()) {
                    continue;
                }

                auto tiles = deserialize_results(payload);
                all_results.insert(all_results.end(),
                                   std::make_move_iterator(tiles.begin()),
                                   std::make_move_iterator(tiles.end()));

                active_task.completed = true;
                completed_workers++;

                LOG_INFO("Coordinator", "Received " + std::to_string(tiles.size()) +
                         " tiles from worker " + active_task.worker_id);
            } else if (header->type == MessageType::MSG_TASK_PROGRESS) {
                std::vector<uint8_t> payload(header->payload_length);
                worker_it->second.socket->receive_all(payload.data(), payload.size());
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return all_results;
}

auto Coordinator::deserialize_results(const std::vector<uint8_t>& data)
    -> std::vector<ProcessedTile> {

    std::vector<ProcessedTile> results;

    if (data.size() < sizeof(uint32_t)) {
        return results;
    }

    uint32_t tile_count;
    std::memcpy(&tile_count, data.data(), sizeof(tile_count));

    size_t offset = sizeof(uint32_t);

    for (uint32_t i = 0; i < tile_count && offset < data.size(); ++i) {
        if (offset + sizeof(uint32_t) * 6 + sizeof(uint8_t) + sizeof(uint32_t) > data.size()) {
            break;
        }

        ProcessedTile tile;
        std::memcpy(&tile.tile_x, data.data() + offset, sizeof(tile.tile_x));
        offset += sizeof(tile.tile_x);
        std::memcpy(&tile.tile_y, data.data() + offset, sizeof(tile.tile_y));
        offset += sizeof(tile.tile_y);
        std::memcpy(&tile.original_width, data.data() + offset, sizeof(tile.original_width));
        offset += sizeof(tile.original_width);
        std::memcpy(&tile.original_height, data.data() + offset, sizeof(tile.original_height));
        offset += sizeof(tile.original_height);
        std::memcpy(&tile.output_width, data.data() + offset, sizeof(tile.output_width));
        offset += sizeof(tile.output_width);
        std::memcpy(&tile.output_height, data.data() + offset, sizeof(tile.output_height));
        offset += sizeof(tile.output_height);
        std::memcpy(&tile.channels, data.data() + offset, sizeof(tile.channels));
        offset += sizeof(tile.channels);

        uint32_t data_size;
        std::memcpy(&data_size, data.data() + offset, sizeof(data_size));
        offset += sizeof(data_size);

        if (offset + data_size > data.size()) {
            break;
        }

        tile.data.resize(data_size);
        std::memcpy(tile.data.data(), data.data() + offset, data_size);
        offset += data_size;

        results.push_back(std::move(tile));
    }

    return results;
}

auto Coordinator::stitch_tiles(const std::vector<ProcessedTile>& tiles,
                                int original_width, int original_height,
                                int channels, int scale_factor)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    int output_width = original_width * scale_factor;
    int output_height = original_height * scale_factor;
    size_t output_size = static_cast<size_t>(output_width) * output_height * channels;

    std::vector<uint8_t> result(output_size, 0);

    for (const auto& tile : tiles) {
        int out_x = static_cast<int>(tile.tile_x) * scale_factor;
        int out_y = static_cast<int>(tile.tile_y) * scale_factor;

        for (uint32_t row = 0; row < tile.output_height; ++row) {
            int dst_y = out_y + static_cast<int>(row);
            if (dst_y >= output_height) continue;

            int dst_offset = (dst_y * output_width + out_x) * channels;
            int src_offset = row * tile.output_width * tile.channels;

            size_t copy_size = std::min(static_cast<size_t>(tile.output_width * tile.channels),
                                        static_cast<size_t>((output_width - out_x) * channels));

            if (dst_offset >= 0 && dst_offset + static_cast<int>(copy_size) <= static_cast<int>(output_size)) {
                std::memcpy(result.data() + dst_offset,
                            tile.data.data() + src_offset,
                            copy_size);
            }
        }
    }

    return result;
}

} // namespace crossscale
