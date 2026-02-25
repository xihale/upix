#include "coordinator.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include <thread>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <optional>
#include <numeric>

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
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    accept_thread_ = std::thread(&Coordinator::accept_loop, this);
    
    LOG_INFO("Coordinator", "Server started on port " + std::to_string(port_));
    return {};
}

void Coordinator::stop() {
    if (!running_ && !server_) {
        return;
    }

    LOG_INFO("Coordinator", "Stopping coordinator...");
    running_ = false;

    if (server_) {
        server_->stop();
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.clear();
    }
    
    if (server_) {
        server_.reset();
    }

    LOG_INFO("Coordinator", "Coordinator stopped");
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
                                   int scale_factor,
                                   const ProcessingConfig& config)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    std::lock_guard<std::mutex> lock(workers_mutex_);

    if (workers_.empty()) {
        return CoordinatorError{2, "No workers connected"};
    }

    LOG_INFO(
        "Coordinator",
        "Starting distributed task model=" + model_name +
            " image=" + std::to_string(width) + "x" + std::to_string(height) +
            " channels=" + std::to_string(channels) +
            " tile=" + std::to_string(config.tile_size) +
            " overlap=" + std::to_string(config.tile_overlap) +
            " workers=" + std::to_string(workers_.size())
    );

    size_t max_tile_bytes = static_cast<size_t>(config.tile_size) *
                            config.tile_size *
                            channels *
                            sizeof(float) *
                            static_cast<size_t>(scale_factor) *
                            static_cast<size_t>(scale_factor);
    max_tile_bytes *= 2;

    for (auto& [id, conn] : workers_) {
        conn.socket->set_buffer_size(max_tile_bytes);
    }

    const int tile_size = config.tile_size;
    const int overlap = config.tile_overlap;
    const int tiles_x = (width + tile_size - 1) / tile_size;
    const int tiles_y = (height + tile_size - 1) / tile_size;
    const int total_tiles = tiles_x * tiles_y;

    if (on_task_progress_ && total_tiles > 0) {
        on_task_progress_(0.0f);
    }

    LOG_INFO("Coordinator", "Splitting image into " + std::to_string(total_tiles) +
             " tiles (" + std::to_string(tiles_x) + "x" + std::to_string(tiles_y) + ")");

    std::vector<uint32_t> pending_tile_indices(static_cast<size_t>(total_tiles));
    std::iota(pending_tile_indices.begin(), pending_tile_indices.end(), 0);

    std::vector<ProcessedTile> all_results;
    all_results.reserve(static_cast<size_t>(total_tiles));

    task_cancelled_ = false;
    size_t wave_index = 0;

    while (!pending_tile_indices.empty()) {
        if (task_cancelled_) {
            return CoordinatorError{6, "Task cancelled"};
        }

        wave_index += 1;
        const uint32_t pending_count = static_cast<uint32_t>(pending_tile_indices.size());
        auto assignments = scheduler_.assign_tiles(pending_count, model_name);
        if (assignments.empty()) {
            LOG_WARNING(
                "Coordinator",
                "Scheduler returned no alive assignments, refreshing worker heartbeats and retrying"
            );
            const auto now = std::chrono::steady_clock::now();
            for (auto& [worker_id, connection] : workers_) {
                connection.profile.last_heartbeat = now;
                scheduler_.register_device(connection.profile);
            }
            assignments = scheduler_.assign_tiles(pending_count, model_name);
            if (assignments.empty()) {
                return CoordinatorError{3, "Failed to assign tasks"};
            }
        }

        active_tasks_.clear();
        pending_results_.clear();

        std::string task_id = crypto::generate_uuid();
        size_t dispatched_workers = 0;

        LOG_INFO(
            "Coordinator",
            "Dispatch wave " + std::to_string(wave_index) +
                " pending_tiles=" + std::to_string(pending_tile_indices.size()) +
                " candidate_workers=" + std::to_string(assignments.size())
        );

        for (const auto& assignment : assignments) {
            auto worker_it = workers_.find(assignment.device_id);
            if (worker_it == workers_.end()) {
                continue;
            }

            std::vector<uint32_t> actual_tile_indices;
            actual_tile_indices.reserve(assignment.assigned_tiles.size());
            for (uint32_t relative_index : assignment.assigned_tiles) {
                if (relative_index < pending_tile_indices.size()) {
                    actual_tile_indices.push_back(pending_tile_indices[relative_index]);
                }
            }

            if (actual_tile_indices.empty()) {
                continue;
            }

            TaskAssignmentMessage msg{};
            std::strncpy(msg.task_id, task_id.c_str(), sizeof(msg.task_id) - 1);
            std::strncpy(msg.model_name, model_name.c_str(), sizeof(msg.model_name) - 1);
            msg.tile_count = static_cast<uint32_t>(actual_tile_indices.size());

            MessageHeader header{};
            header.type = MessageType::MSG_TASK_ASSIGNMENT;
            header.payload_length = sizeof(msg) + msg.tile_count * sizeof(TileInfo);

            std::vector<uint8_t> payload(header.payload_length);
            std::memcpy(payload.data(), &msg, sizeof(msg));

            size_t offset = sizeof(msg);
            for (uint32_t tile_idx : actual_tile_indices) {
                const int tile_x = static_cast<int>(tile_idx % static_cast<uint32_t>(tiles_x));
                const int tile_y = static_cast<int>(tile_idx / static_cast<uint32_t>(tiles_x));

                const int x = tile_x * tile_size;
                const int y = tile_y * tile_size;
                const int w = std::min(tile_size, width - x);
                const int h = std::min(tile_size, height - y);

                TileInfo tile{};
                tile.x = static_cast<uint32_t>(x);
                tile.y = static_cast<uint32_t>(y);
                tile.width = static_cast<uint32_t>(w);
                tile.height = static_cast<uint32_t>(h);
                tile.overlap = static_cast<uint32_t>(overlap);
                std::memcpy(payload.data() + offset, &tile, sizeof(tile));
                offset += sizeof(tile);
            }

            auto data = Protocol::serialize(header, payload.data());
            auto send_assignment_result = worker_it->second.socket->send_all(data.data(), data.size());
            if (!send_assignment_result.has_value()) {
                LOG_ERROR(
                    "Coordinator",
                    "Failed to send task to worker " + assignment.device_id +
                        ": " + send_assignment_result.error().message
                );
                scheduler_.unregister_device(assignment.device_id);
                workers_.erase(assignment.device_id);
                if (on_device_disconnected_) {
                    on_device_disconnected_(assignment.device_id);
                }
                continue;
            }

            size_t total_batch_size = 0;
            std::vector<ImageTileMessage> tile_msgs;
            tile_msgs.reserve(actual_tile_indices.size());
            std::vector<std::vector<uint8_t>> tile_data_bufs;
            tile_data_bufs.reserve(actual_tile_indices.size());

            for (uint32_t tile_idx : actual_tile_indices) {
                const int tile_x = static_cast<int>(tile_idx % static_cast<uint32_t>(tiles_x));
                const int tile_y = static_cast<int>(tile_idx / static_cast<uint32_t>(tiles_x));
                const int x = tile_x * tile_size;
                const int y = tile_y * tile_size;
                const int w = std::min(tile_size, width - x);
                const int h = std::min(tile_size, height - y);

                size_t tile_data_size = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(channels);
                std::vector<uint8_t> tile_data(tile_data_size);
                for (int row = 0; row < h; ++row) {
                    const int src_y = y + row;
                    const int src_row_offset = (src_y * width + x) * channels;
                    const int dst_row_offset = row * w * channels;
                    std::memcpy(tile_data.data() + dst_row_offset,
                                image_data.data() + src_row_offset,
                                static_cast<size_t>(w * channels));
                }

                ImageTileMessage tile_msg{};
                std::strncpy(tile_msg.task_id, task_id.c_str(), sizeof(tile_msg.task_id) - 1);
                tile_msg.tile_index = tile_idx;
                tile_msg.tile_x = static_cast<uint32_t>(x);
                tile_msg.tile_y = static_cast<uint32_t>(y);
                tile_msg.width = static_cast<uint32_t>(w);
                tile_msg.height = static_cast<uint32_t>(h);
                tile_msg.channels = static_cast<uint8_t>(channels);
                tile_msg.pixel_format = 0;
                tile_msg.data_length = static_cast<uint32_t>(tile_data_size);

                total_batch_size += sizeof(tile_msg) + tile_data_size;
                tile_msgs.push_back(tile_msg);
                tile_data_bufs.push_back(std::move(tile_data));
            }

            std::vector<uint8_t> batch_payload(total_batch_size);
            size_t batch_offset = 0;
            for (size_t i = 0; i < tile_msgs.size(); ++i) {
                std::memcpy(batch_payload.data() + batch_offset, &tile_msgs[i], sizeof(tile_msgs[i]));
                batch_offset += sizeof(tile_msgs[i]);
                std::memcpy(batch_payload.data() + batch_offset,
                            tile_data_bufs[i].data(),
                            tile_data_bufs[i].size());
                batch_offset += tile_data_bufs[i].size();
            }

            MessageHeader batch_header{};
            batch_header.type = MessageType::MSG_TILES_BATCH;
            batch_header.payload_length = static_cast<uint32_t>(total_batch_size);
            auto batch_msg = Protocol::serialize(batch_header, batch_payload.data());
            auto send_batch_result = worker_it->second.socket->send_all(batch_msg.data(), batch_msg.size());
            if (!send_batch_result.has_value()) {
                LOG_ERROR(
                    "Coordinator",
                    "Failed to send tile batch to worker " + assignment.device_id +
                        ": " + send_batch_result.error().message
                );
                scheduler_.unregister_device(assignment.device_id);
                workers_.erase(assignment.device_id);
                if (on_device_disconnected_) {
                    on_device_disconnected_(assignment.device_id);
                }
                continue;
            }

            ActiveTask active_task;
            active_task.task_id = task_id;
            active_task.worker_id = assignment.device_id;
            active_task.tile_indices = actual_tile_indices;
            active_task.completed = false;
            active_tasks_[task_id + "_" + assignment.device_id] = std::move(active_task);
            dispatched_workers += 1;
        }

        if (dispatched_workers == 0) {
            return CoordinatorError{4, "No active workers accepted the current dispatch wave"};
        }

        auto wave_results = collect_results(task_id, dispatched_workers, static_cast<int>(pending_tile_indices.size()));
        if (!wave_results.has_value()) {
            return CoordinatorError{4, "Failed to collect results: " + wave_results.error().message};
        }

        if (!wave_results->empty()) {
            all_results.insert(
                all_results.end(),
                std::make_move_iterator(wave_results->begin()),
                std::make_move_iterator(wave_results->end())
            );
        }

        std::vector<uint32_t> completed_tile_indices;
        completed_tile_indices.reserve(wave_results->size());
        for (const auto& processed_tile : *wave_results) {
            const int completed_x_index = static_cast<int>(processed_tile.tile_x) / tile_size;
            const int completed_y_index = static_cast<int>(processed_tile.tile_y) / tile_size;
            if (completed_x_index < 0 || completed_x_index >= tiles_x ||
                completed_y_index < 0 || completed_y_index >= tiles_y) {
                continue;
            }
            const uint32_t tile_index = static_cast<uint32_t>(completed_y_index * tiles_x + completed_x_index);
            completed_tile_indices.push_back(tile_index);
        }

        std::sort(completed_tile_indices.begin(), completed_tile_indices.end());
        completed_tile_indices.erase(std::unique(completed_tile_indices.begin(), completed_tile_indices.end()),
                                     completed_tile_indices.end());

        if (completed_tile_indices.empty()) {
            return CoordinatorError{4, "No completed tiles reported in dispatch wave"};
        }

        pending_tile_indices.erase(
            std::remove_if(
                pending_tile_indices.begin(),
                pending_tile_indices.end(),
                [&](uint32_t tile_index) {
                    return std::binary_search(completed_tile_indices.begin(),
                                              completed_tile_indices.end(),
                                              tile_index);
                }
            ),
            pending_tile_indices.end()
        );

        LOG_INFO(
            "Coordinator",
            "Dispatch wave " + std::to_string(wave_index) +
                " completed_tiles=" + std::to_string(completed_tile_indices.size()) +
                " remaining_tiles=" + std::to_string(pending_tile_indices.size())
        );

        if (on_task_progress_ && total_tiles > 0) {
            const int completed_tiles = total_tiles - static_cast<int>(pending_tile_indices.size());
            const float overall_progress = static_cast<float>(completed_tiles) / static_cast<float>(total_tiles);
            on_task_progress_(std::clamp(overall_progress, 0.0f, 1.0f));
        }
    }

    LOG_INFO("Coordinator", "Stitching " + std::to_string(all_results.size()) + " tiles into final image");

    auto stitched = stitch_tiles(all_results, width, height, channels, scale_factor);
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
    conn.device_id = crypto::generate_uuid().substr(0, 35);

    auto read_payload = [&](uint32_t payload_length) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> payload(payload_length);
        if (payload_length == 0) {
            return payload;
        }
        auto payload_result = conn.socket->receive_all(payload.data(), payload.size());
        if (!payload_result.has_value()) {
            return std::nullopt;
        }
        return payload;
    };

    auto parse_device_profile = [](const DeviceProfileMessage& msg, const std::string& fallback_device_id) {
        DeviceProfile profile;
        profile.device_id = fallback_device_id;
        profile.device_name = std::string(msg.device_name);
        profile.type = static_cast<DeviceType>(msg.device_type);
        profile.tiles_per_second = std::max(msg.tiles_per_second, 1.0f);
        profile.recommended_tile_size = static_cast<int>(msg.recommended_tile_size);
        profile.max_batch_size = static_cast<size_t>(msg.max_batch_size);
        profile.ip_address = std::string(msg.ip_address);
        profile.port = msg.port;
        profile.last_heartbeat = std::chrono::steady_clock::now();
        return profile;
    };
    
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
        auto payload = read_payload(header->payload_length);
        if (!payload.has_value() || payload->size() < sizeof(AuthRequest)) {
            return;
        }
        
        AuthRequest* auth_req = reinterpret_cast<AuthRequest*>(payload->data());
        if (!authenticate(conn, auth_req->auth_key)) {
            AuthResponse resp{};
            resp.authenticated = 0;
            resp.device_id[0] = '\0';
            
            MessageHeader resp_header{};
            resp_header.type = MessageType::MSG_AUTH_RESPONSE;
            resp_header.payload_length = sizeof(resp);
            
            auto data = Protocol::serialize(resp_header, &resp);
            conn.socket->send_all(data.data(), data.size());
            return;
        }
        
        AuthResponse resp{};
        resp.authenticated = 1;
        std::strncpy(resp.device_id, conn.device_id.c_str(), sizeof(resp.device_id) - 1);
        resp.device_id[sizeof(resp.device_id) - 1] = '\0';
        
        MessageHeader resp_header{};
        resp_header.type = MessageType::MSG_AUTH_RESPONSE;
        resp_header.payload_length = sizeof(resp);
        
        auto data = Protocol::serialize(resp_header, &resp);
        conn.socket->send_all(data.data(), data.size());
        
        conn.authenticated = true;
        conn.profile.device_id = conn.device_id;
        conn.profile.device_name = "CLI-Worker";
        conn.profile.type = DeviceType::CPU;
        conn.profile.tiles_per_second = 1.0f;
        conn.profile.recommended_tile_size = 256;
        conn.profile.max_batch_size = 1;
        conn.profile.ip_address = conn.socket->peer_address();
        conn.profile.port = conn.socket->peer_port();
        conn.profile.last_heartbeat = std::chrono::steady_clock::now();

        conn.socket->set_timeout(5000);
        std::vector<uint8_t> profile_header_buffer(Protocol::HEADER_SIZE);
        auto profile_header_result = conn.socket->receive_all(profile_header_buffer.data(), profile_header_buffer.size());
        if (profile_header_result.has_value()) {
            auto profile_header = Protocol::parse_header(profile_header_buffer.data(), profile_header_buffer.size());
            if (profile_header && profile_header->type == MessageType::MSG_DEVICE_PROFILE) {
                auto profile_payload = read_payload(profile_header->payload_length);
                if (profile_payload.has_value() && profile_payload->size() >= sizeof(DeviceProfileMessage)) {
                    const DeviceProfileMessage* msg = reinterpret_cast<const DeviceProfileMessage*>(profile_payload->data());
                    conn.profile = parse_device_profile(*msg, conn.device_id);
                }
            } else if (profile_header && profile_header->type == MessageType::MSG_CLIENT_SUBMIT) {
                auto submit_payload = read_payload(profile_header->payload_length);
                if (!submit_payload.has_value() || submit_payload->size() < sizeof(ClientTaskSubmit)) {
                    LOG_ERROR("Coordinator", "Authenticated client submit payload is invalid");
                    return;
                }

                ClientTaskSubmit* task = reinterpret_cast<ClientTaskSubmit*>(submit_payload->data());
                uint32_t img_width = task->image_width;
                uint32_t img_height = task->image_height;
                uint8_t channels = task->channels;
                std::string model_name(task->model_name);
                uint32_t tile_size = task->tile_size;
                uint32_t tile_overlap = task->tile_overlap;
                uint32_t image_data_len = task->image_data_length;

                std::vector<uint8_t> image_data(image_data_len);
                auto image_receive = conn.socket->receive_all(image_data.data(), image_data_len);
                if (!image_receive.has_value()) {
                    LOG_ERROR(
                        "Coordinator",
                        "Failed to receive authenticated client image payload: " + image_receive.error().message
                    );
                    return;
                }

                LOG_INFO(
                    "Coordinator",
                    "Received authenticated client task: " +
                        std::to_string(img_width) + "x" + std::to_string(img_height) +
                        " model=" + model_name
                );

                ProcessingConfig config;
                config.tile_size = static_cast<int>(tile_size);
                config.tile_overlap = static_cast<int>(tile_overlap);

                auto process_result = distribute_task(
                    image_data,
                    static_cast<int>(img_width),
                    static_cast<int>(img_height),
                    static_cast<int>(channels),
                    model_name,
                    2,
                    config
                );

                ClientTaskResponse response{};
                if (process_result.has_value()) {
                    response.success = 1;
                    response.result_data_length = static_cast<uint32_t>(process_result->size());

                    MessageHeader response_header{};
                    response_header.type = MessageType::MSG_CLIENT_RESPONSE;
                    response_header.payload_length = sizeof(response);

                    auto response_header_data = Protocol::serialize(response_header, &response);
                    auto send_header_result = conn.socket->send_all(response_header_data.data(), response_header_data.size());
                    if (!send_header_result.has_value()) {
                        LOG_ERROR(
                            "Coordinator",
                            "Failed to send authenticated client response header: " + send_header_result.error().message
                        );
                        return;
                    }

                    auto send_result_data = conn.socket->send_all(process_result->data(), process_result->size());
                    if (!send_result_data.has_value()) {
                        LOG_ERROR(
                            "Coordinator",
                            "Failed to send authenticated client result data: " + send_result_data.error().message
                        );
                        return;
                    }

                    LOG_INFO(
                        "Coordinator",
                        "Authenticated client task completed, sent " +
                            std::to_string(process_result->size()) + " bytes"
                    );
                } else {
                    response.success = 0;
                    response.result_data_length = 0;
                    std::strncpy(
                        response.error_message,
                        process_result.error().message.c_str(),
                        sizeof(response.error_message) - 1
                    );

                    MessageHeader response_header{};
                    response_header.type = MessageType::MSG_CLIENT_RESPONSE;
                    response_header.payload_length = sizeof(response);

                    auto response_header_data = Protocol::serialize(response_header, &response);
                    auto send_header_result = conn.socket->send_all(response_header_data.data(), response_header_data.size());
                    if (!send_header_result.has_value()) {
                        LOG_ERROR(
                            "Coordinator",
                            "Failed to send authenticated client failure response: " + send_header_result.error().message
                        );
                        return;
                    }
                    LOG_ERROR(
                        "Coordinator",
                        "Authenticated client task failed: " + process_result.error().message
                    );
                }
                return;
            } else if (profile_header) {
                auto ignored_payload = read_payload(profile_header->payload_length);
                (void)ignored_payload;
            }
        }

        const std::string worker_id = conn.device_id;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            scheduler_.register_device(conn.profile);
            workers_[worker_id] = std::move(conn);
            auto it = workers_.find(worker_id);
            if (it != workers_.end()) {
                send_model_list(it->second);
            }
        }
        
        LOG_INFO("Coordinator", "Worker connected: " + worker_id);
        
        if (on_device_connected_) {
            std::optional<DeviceProfile> connected_profile;
            {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                auto it = workers_.find(worker_id);
                if (it != workers_.end()) {
                    connected_profile = it->second.profile;
                }
            }
            if (connected_profile.has_value()) {
                on_device_connected_(connected_profile.value());
            }
        }
    }
    else if (header->type == MessageType::MSG_DEVICE_PROFILE) {
        auto ignored_payload = read_payload(header->payload_length);
        (void)ignored_payload;
        LOG_WARNING("Coordinator", "Ignored standalone device profile without authenticated worker context");
    }
    else if (header->type == MessageType::MSG_CLIENT_SUBMIT) {
        std::vector<uint8_t> payload(header->payload_length);
        conn.socket->receive_all(payload.data(), payload.size());
        
        ClientTaskSubmit* task = reinterpret_cast<ClientTaskSubmit*>(payload.data());
        
        uint32_t img_width = task->image_width;
        uint32_t img_height = task->image_height;
        uint8_t channels = task->channels;
        std::string model_name(task->model_name);
        uint32_t tile_size = task->tile_size;
        uint32_t tile_overlap = task->tile_overlap;
        uint32_t image_data_len = task->image_data_length;
        
        std::vector<uint8_t> image_data(image_data_len);
        conn.socket->receive_all(image_data.data(), image_data_len);
        
        LOG_INFO("Coordinator", "Received client task: " + 
                 std::to_string(img_width) + "x" + std::to_string(img_height) +
                 " model=" + model_name);
        
        ProcessingConfig config;
        config.tile_size = static_cast<int>(tile_size);
        config.tile_overlap = static_cast<int>(tile_overlap);
        
        auto result = distribute_task(
            image_data, 
            static_cast<int>(img_width),
            static_cast<int>(img_height),
            static_cast<int>(channels),
            model_name,
            2,
            config
        );
        
        ClientTaskResponse resp{};
        if (result.has_value()) {
            resp.success = 1;
            resp.result_data_length = static_cast<uint32_t>(result->size());
            
            MessageHeader resp_header{};
            resp_header.type = MessageType::MSG_CLIENT_RESPONSE;
            resp_header.payload_length = sizeof(resp);
            
            auto header_data = Protocol::serialize(resp_header, &resp);
            auto send_header_result = conn.socket->send_all(header_data.data(), header_data.size());
            if (!send_header_result.has_value()) {
                LOG_ERROR(
                    "Coordinator",
                    "Failed to send client response header: " + send_header_result.error().message
                );
                return;
            }

            auto send_result_data = conn.socket->send_all(result->data(), result->size());
            if (!send_result_data.has_value()) {
                LOG_ERROR(
                    "Coordinator",
                    "Failed to send client result data: " + send_result_data.error().message
                );
                return;
            }
            
            LOG_INFO("Coordinator", "Client task completed, sent " + 
                     std::to_string(result->size()) + " bytes");
        } else {
            resp.success = 0;
            resp.result_data_length = 0;
            std::strncpy(resp.error_message, result.error().message.c_str(), 
                         sizeof(resp.error_message) - 1);
            
            MessageHeader resp_header{};
            resp_header.type = MessageType::MSG_CLIENT_RESPONSE;
            resp_header.payload_length = sizeof(resp);
            
            auto header_data = Protocol::serialize(resp_header, &resp);
            auto send_header_result = conn.socket->send_all(header_data.data(), header_data.size());
            if (!send_header_result.has_value()) {
                LOG_ERROR(
                    "Coordinator",
                    "Failed to send client failure response: " + send_header_result.error().message
                );
                return;
            }
            
            LOG_ERROR("Coordinator", "Client task failed: " + result.error().message);
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

    MessageHeader header{};
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

    MessageHeader header{};
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

    MessageHeader header{};
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
                const int error_code = result.error().code;
                const bool timeout_error = (error_code == EAGAIN || error_code == EWOULDBLOCK);
                if (!timeout_error) {
                    LOG_WARNING(
                        "Coordinator",
                        "Worker disconnected while collecting results: " + active_task.worker_id +
                            " reason=" + result.error().message
                    );
                    scheduler_.unregister_device(active_task.worker_id);
                    workers_.erase(worker_it);
                    active_task.completed = true;
                    completed_workers++;
                    if (on_device_disconnected_) {
                        on_device_disconnected_(active_task.worker_id);
                    }
                }
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

                worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
                scheduler_.register_device(worker_it->second.profile);

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
                auto progress_receive = worker_it->second.socket->receive_all(payload.data(), payload.size());
                if (!progress_receive.has_value()) {
                    continue;
                }
                worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
                scheduler_.register_device(worker_it->second.profile);

                struct ProgressMessage {
                    char task_id[36];
                    float progress;
                };

                if (payload.size() >= sizeof(ProgressMessage)) {
                    const auto* progress_message = reinterpret_cast<const ProgressMessage*>(payload.data());
                    const float progress = std::clamp(progress_message->progress, 0.0f, 1.0f);
                    const int progress_percent = static_cast<int>(progress * 100.0f);
                    LOG_DEBUG(
                        "Coordinator",
                        "Worker progress " + active_task.worker_id + " task=" +
                            std::string(progress_message->task_id) +
                            " progress=" + std::to_string(progress_percent) + "%"
                    );
                }
            } else if (header->type == MessageType::MSG_TASK_ACCEPT) {
                std::vector<uint8_t> payload(header->payload_length);
                auto accept_receive = worker_it->second.socket->receive_all(payload.data(), payload.size());
                if (!accept_receive.has_value()) {
                    continue;
                }

                worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
                scheduler_.register_device(worker_it->second.profile);

                std::string accepted_task_id;
                if (!payload.empty()) {
                    const char* raw_task_id = reinterpret_cast<const char*>(payload.data());
                    accepted_task_id.assign(raw_task_id, strnlen(raw_task_id, payload.size()));
                }
                LOG_INFO(
                    "Coordinator",
                    "Worker accepted task " +
                        (accepted_task_id.empty() ? std::string("<unknown>") : accepted_task_id) +
                        " worker=" + active_task.worker_id
                );
            } else if (header->type == MessageType::MSG_HEARTBEAT) {
                worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
                scheduler_.register_device(worker_it->second.profile);
            } else if (header->type == MessageType::MSG_MODEL_REQUEST) {
                std::vector<uint8_t> payload(header->payload_length);
                auto request_receive = worker_it->second.socket->receive_all(payload.data(), payload.size());
                if (!request_receive.has_value()) {
                    LOG_WARNING(
                        "Coordinator",
                        "Failed to receive model request payload from worker " +
                            active_task.worker_id + ": " + request_receive.error().message
                    );
                    continue;
                }

                auto model_request_result = handle_model_request(worker_it->second, payload);
                if (!model_request_result.has_value()) {
                    LOG_WARNING(
                        "Coordinator",
                        "Failed to serve model request for worker " + active_task.worker_id +
                            ": " + model_request_result.error().message
                    );
                } else {
                    worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
                    scheduler_.register_device(worker_it->second.profile);
                }
            } else if (header->type == MessageType::MSG_DISCONNECT) {
                LOG_WARNING("Coordinator", "Worker requested disconnect: " + active_task.worker_id);
                scheduler_.unregister_device(active_task.worker_id);
                workers_.erase(worker_it);
                active_task.completed = true;
                completed_workers++;
                if (on_device_disconnected_) {
                    on_device_disconnected_(active_task.worker_id);
                }
            } else {
                if (header->payload_length > 0) {
                    std::vector<uint8_t> ignored_payload(header->payload_length);
                    worker_it->second.socket->receive_all(ignored_payload.data(), ignored_payload.size());
                }
                LOG_WARNING(
                    "Coordinator",
                    "Ignoring unexpected message type " +
                        std::to_string(static_cast<int>(header->type)) +
                        " from worker " + active_task.worker_id
                );
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
