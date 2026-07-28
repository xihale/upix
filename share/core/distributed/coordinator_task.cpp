#include "coordinator.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "utils/crypto.hpp"
#include <thread>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <deque>
#include <unordered_set>

namespace crossscale {

auto Coordinator::distribute_task(const std::vector<uint8_t>& image_data,
                                   int width, int height, int channels,
                                   const std::string& model_name,
                                   int scale_factor,
                                   const ProcessingConfig& config)
    -> expected<std::vector<uint8_t>, CoordinatorError> {

    std::lock_guard<std::mutex> lock(workers_mutex_);

    if (workers_.empty()) {
        return std::unexpected(CoordinatorError{2, "No workers connected"});
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
        // Use a short receive timeout so the dispatch loop can poll for results
        // without blocking indefinitely on any single worker.
        conn.socket->set_timeout(200);
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

    // --- Continuous dispatch: work-pool with per-worker re-dispatch, no global barrier ---
    // Tiles flow: pending pool -> dispatched to a worker (inflight) -> results collected.
    // When a worker finishes its batch, it immediately gets more tiles from the pool.
    // If a worker disconnects, its inflight tiles go back to the pool (retried).
    std::deque<uint32_t> pending_pool;
    for (int i = 0; i < total_tiles; ++i) {
        pending_pool.push_back(static_cast<uint32_t>(i));
    }

    std::unordered_map<std::string, ActiveTask> inflight;  // worker_id -> active task
    std::vector<ProcessedTile> all_results;
    all_results.reserve(static_cast<size_t>(total_tiles));
    std::unordered_set<uint32_t> completed_tile_set;

    task_cancelled_ = false;

    // Helper: compute tile geometry for a linear tile index.
    auto tile_geometry = [&](uint32_t tile_idx) {
        const int tx = static_cast<int>(tile_idx % tiles_x);
        const int ty = static_cast<int>(tile_idx / tiles_x);
        const int x = tx * tile_size;
        const int y = ty * tile_size;
        const int w = std::min(tile_size, width - x);
        const int h = std::min(tile_size, height - y);
        struct Geo { int x, y, w, h; } g{x, y, w, h};
        return g;
    };

    // Helper: dispatch a batch of tiles to a specific worker.
    // Returns the number of tiles dispatched (0 on failure).
    auto dispatch_to_worker = [&](WorkerConnection& conn,
                                   const std::string& worker_id,
                                   std::vector<uint32_t>& tiles) -> size_t {
        if (tiles.empty()) return 0;

        const std::string task_id = crypto::generate_uuid();

        // Build task assignment message
        TaskAssignmentMessage msg{};
        std::strncpy(msg.task_id, task_id.c_str(), sizeof(msg.task_id) - 1);
        std::strncpy(msg.model_name, model_name.c_str(), sizeof(msg.model_name) - 1);
        msg.tile_count = static_cast<uint32_t>(tiles.size());

        MessageHeader header{};
        header.type = MessageType::MSG_TASK_ASSIGNMENT;
        header.payload_length = sizeof(msg) + msg.tile_count * sizeof(TileInfo);

        std::vector<uint8_t> payload(header.payload_length);
        std::memcpy(payload.data(), &msg, sizeof(msg));
        size_t offset = sizeof(msg);
        for (uint32_t tile_idx : tiles) {
            auto g = tile_geometry(tile_idx);
            TileInfo ti{};
            ti.x = static_cast<uint32_t>(g.x);
            ti.y = static_cast<uint32_t>(g.y);
            ti.width = static_cast<uint32_t>(g.w);
            ti.height = static_cast<uint32_t>(g.h);
            ti.overlap = static_cast<uint32_t>(overlap);
            std::memcpy(payload.data() + offset, &ti, sizeof(ti));
            offset += sizeof(ti);
        }

        auto data = Protocol::serialize(header, payload.data());
        auto send_res = conn.socket->send_all(data.data(), data.size());
        if (!send_res.has_value()) return 0;

        // Build and send tile pixel data batch
        size_t total_batch_size = 0;
        std::vector<ImageTileMessage> tile_msgs;
        tile_msgs.reserve(tiles.size());
        std::vector<std::vector<uint8_t>> tile_data_bufs;
        tile_data_bufs.reserve(tiles.size());

        for (uint32_t tile_idx : tiles) {
            auto g = tile_geometry(tile_idx);
            size_t tile_data_size = static_cast<size_t>(g.w) * static_cast<size_t>(g.h) * static_cast<size_t>(channels);
            std::vector<uint8_t> tile_data(tile_data_size);
            for (int row = 0; row < g.h; ++row) {
                const int src_row_offset = ((g.y + row) * width + g.x) * channels;
                const int dst_row_offset = row * g.w * channels;
                std::memcpy(tile_data.data() + dst_row_offset,
                            image_data.data() + src_row_offset,
                            static_cast<size_t>(g.w * channels));
            }

            ImageTileMessage tm{};
            std::strncpy(tm.task_id, task_id.c_str(), sizeof(tm.task_id) - 1);
            tm.tile_index = tile_idx;
            tm.tile_x = static_cast<uint32_t>(g.x);
            tm.tile_y = static_cast<uint32_t>(g.y);
            tm.width = static_cast<uint32_t>(g.w);
            tm.height = static_cast<uint32_t>(g.h);
            tm.channels = static_cast<uint8_t>(channels);
            tm.pixel_format = 0;
            tm.data_length = static_cast<uint32_t>(tile_data_size);

            total_batch_size += sizeof(tm) + tile_data_size;
            tile_msgs.push_back(tm);
            tile_data_bufs.push_back(std::move(tile_data));
        }

        std::vector<uint8_t> batch_payload(total_batch_size);
        size_t batch_offset = 0;
        for (size_t i = 0; i < tile_msgs.size(); ++i) {
            std::memcpy(batch_payload.data() + batch_offset, &tile_msgs[i], sizeof(tile_msgs[i]));
            batch_offset += sizeof(tile_msgs[i]);
            std::memcpy(batch_payload.data() + batch_offset,
                        tile_data_bufs[i].data(), tile_data_bufs[i].size());
            batch_offset += tile_data_bufs[i].size();
        }

        MessageHeader batch_header{};
        batch_header.type = MessageType::MSG_TILES_BATCH;
        batch_header.payload_length = static_cast<uint32_t>(total_batch_size);
        // IMPORTANT: do NOT send the tile batch yet. It is held back until the
        // worker accepts the task — see the dispatch loop. This lets the worker
        // request the model (MSG_MODEL_REQUEST) between ACCEPT and the batch
        // without the batch bytes colliding with the model-request exchange.
        auto batch_msg = Protocol::serialize(batch_header, batch_payload.data());

        ActiveTask at;
        at.task_id = task_id;
        at.worker_id = worker_id;
        at.tile_indices = tiles;
        at.completed = false;
        at.dispatched_at = std::chrono::steady_clock::now();
        at.pending_batch = std::move(batch_msg);
        at.batch_sent = false;
        inflight[worker_id] = std::move(at);

        return tiles.size();
    };

    // Helper: handle a worker disconnection — requeue its inflight tiles.
    auto handle_disconnect = [&](const std::string& worker_id) {
        auto it = inflight.find(worker_id);
        if (it != inflight.end()) {
            for (uint32_t ti : it->second.tile_indices) {
                if (completed_tile_set.find(ti) == completed_tile_set.end()) {
                    pending_pool.push_back(ti);
                }
            }
            inflight.erase(it);
        }
        scheduler_.unregister_device(worker_id);
        workers_.erase(worker_id);
        if (on_device_disconnected_) {
            on_device_disconnected_(worker_id);
        }
    };

    // Per-tile timeout: if a worker's batch hasn't completed in this duration,
    // requeue its tiles and drop the worker.
    constexpr auto TILE_TIMEOUT = std::chrono::seconds(120);

    while (!pending_pool.empty() || !inflight.empty()) {
        if (task_cancelled_) {
            return std::unexpected(CoordinatorError{6, "Task cancelled"});
        }

        // --- Phase 1: Dispatch tiles to idle workers ---
        if (!pending_pool.empty()) {
            auto assignments = scheduler_.assign_tiles(
                static_cast<uint32_t>(pending_pool.size()), model_name);
            if (assignments.empty()) {
                // Refresh heartbeats and retry once
                const auto now = std::chrono::steady_clock::now();
                for (auto& [wid, conn] : workers_) {
                    conn.profile.last_heartbeat = now;
                    scheduler_.register_device(conn.profile);
                }
                assignments = scheduler_.assign_tiles(
                    static_cast<uint32_t>(pending_pool.size()), model_name);
            }

            for (const auto& assignment : assignments) {
                if (pending_pool.empty()) break;

                auto worker_it = workers_.find(assignment.device_id);
                if (worker_it == workers_.end()) continue;
                if (inflight.find(assignment.device_id) != inflight.end()) continue;  // busy

                // Take tiles from the front of the pool. Give the worker its full
                // assigned share in a single task so it can stream results back
                // without round-tripping per batch — the per-task tile count is
                // governed by the scheduler's weight-based assignment, and
                // max_batch_size only caps how many tiles a worker processes
                // concurrently inside that task (a throughput hint, not a hard
                // cap on task size). Capping task size here fragments a small
                // job across multiple ASSIGNMENT/ACCEPT/batch/result round
                // trips and races the dispatch loop.
                size_t take = std::min(assignment.assigned_tiles.size(), pending_pool.size());
                if (take == 0) continue;

                std::vector<uint32_t> batch;
                batch.reserve(take);
                for (size_t i = 0; i < take && !pending_pool.empty(); ++i) {
                    batch.push_back(pending_pool.front());
                    pending_pool.pop_front();
                }
                if (batch.empty()) continue;

                size_t dispatched = dispatch_to_worker(worker_it->second, assignment.device_id, batch);
                if (dispatched == 0) {
                    LOG_WARNING("Coordinator", "Failed to dispatch to worker " + assignment.device_id);
                    // Put tiles back and drop worker
                    for (uint32_t ti : batch) pending_pool.push_front(ti);
                    handle_disconnect(assignment.device_id);
                }
            }
        }

        if (workers_.empty() && !pending_pool.empty()) {
            return std::unexpected(CoordinatorError{4, "All workers disconnected with " +
                std::to_string(pending_pool.size()) + " tiles remaining"});
        }

        // --- Phase 2: Poll inflight workers for results ---
        std::vector<std::string> to_remove;
        for (auto& [worker_id, task] : inflight) {
            if (task.completed) continue;

            auto worker_it = workers_.find(worker_id);
            if (worker_it == workers_.end()) {
                to_remove.push_back(worker_id);
                continue;
            }

            // Check tile-level timeout
            auto elapsed = std::chrono::steady_clock::now() - task.dispatched_at;
            if (elapsed > TILE_TIMEOUT) {
                LOG_WARNING("Coordinator", "Worker " + worker_id +
                    " timed out (" + std::to_string(
                        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()) +
                    "s), requeuing " + std::to_string(task.tile_indices.size()) + " tiles");
                for (uint32_t ti : task.tile_indices) {
                    if (completed_tile_set.find(ti) == completed_tile_set.end()) {
                        pending_pool.push_back(ti);
                    }
                }
                to_remove.push_back(worker_id);
                scheduler_.unregister_device(worker_id);
                workers_.erase(worker_it);
                if (on_device_disconnected_) on_device_disconnected_(worker_id);
                continue;
            }

            auto& socket = worker_it->second.socket;
            std::vector<uint8_t> header_buffer(Protocol::HEADER_SIZE);
            auto recv_res = socket->receive_all(header_buffer.data(), header_buffer.size());

            if (!recv_res.has_value()) {
                const int err = recv_res.error().code;
                const bool timeout_err = (err == EAGAIN || err == EWOULDBLOCK);
                if (!timeout_err) {
                    LOG_WARNING("Coordinator", "Worker " + worker_id +
                        " disconnected: " + recv_res.error().message);
                    for (uint32_t ti : task.tile_indices) {
                        if (completed_tile_set.find(ti) == completed_tile_set.end()) {
                            pending_pool.push_back(ti);
                        }
                    }
                    to_remove.push_back(worker_id);
                    scheduler_.unregister_device(worker_id);
                    workers_.erase(worker_it);
                    if (on_device_disconnected_) on_device_disconnected_(worker_id);
                }
                continue;
            }

            auto hdr = Protocol::parse_header(header_buffer.data(), header_buffer.size());
            if (!hdr) continue;

            worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
            scheduler_.register_device(worker_it->second.profile);

            if (hdr->type == MessageType::MSG_TASK_RESULT) {
                std::vector<uint8_t> payload(hdr->payload_length);
                auto pay_res = socket->receive_all(payload.data(), payload.size());
                if (!pay_res.has_value()) continue;

                auto tiles = deserialize_results(payload);
                LOG_INFO("Coordinator", "Received " + std::to_string(tiles.size()) +
                         " tiles from worker " + worker_id);

                for (const auto& pt : tiles) {
                    const int cx = static_cast<int>(pt.tile_x) / tile_size;
                    const int cy = static_cast<int>(pt.tile_y) / tile_size;
                    if (cx >= 0 && cx < tiles_x && cy >= 0 && cy < tiles_y) {
                        completed_tile_set.insert(static_cast<uint32_t>(cy * tiles_x + cx));
                    }
                }
                all_results.insert(all_results.end(),
                                   std::make_move_iterator(tiles.begin()),
                                   std::make_move_iterator(tiles.end()));

                task.completed = true;
                to_remove.push_back(worker_id);

                if (on_task_progress_ && total_tiles > 0) {
                    on_task_progress_(std::clamp(
                        static_cast<float>(completed_tile_set.size()) / total_tiles, 0.0f, 1.0f));
                }
            } else if (hdr->type == MessageType::MSG_TASK_ACCEPT) {
                // Worker accepted — now it is safe to send the tile batch.
                // The worker may have requested the model between reading the
                // assignment and sending ACCEPT; that exchange is already done.
                if (!task.batch_sent) {
                    auto send_res = socket->send_all(
                        task.pending_batch.data(), task.pending_batch.size());
                    if (!send_res.has_value()) {
                        LOG_WARNING("Coordinator", "Worker " + worker_id +
                            " disconnected while sending tile batch: " +
                            send_res.error().message);
                        for (uint32_t ti : task.tile_indices) {
                            if (completed_tile_set.find(ti) == completed_tile_set.end()) {
                                pending_pool.push_back(ti);
                            }
                        }
                        to_remove.push_back(worker_id);
                        scheduler_.unregister_device(worker_id);
                        workers_.erase(worker_it);
                        if (on_device_disconnected_) on_device_disconnected_(worker_id);
                        continue;
                    }
                    task.batch_sent = true;
                }
            } else if (hdr->type == MessageType::MSG_MODEL_REQUEST) {
                // Worker is fetching the model for this task. Serve it, then
                // keep polling — the ACCEPT/batch/result will follow.
                std::vector<uint8_t> mpayload(hdr->payload_length);
                auto mpay_res = socket->receive_all(mpayload.data(), mpayload.size());
                if (mpay_res.has_value()) {
                    auto mr = handle_model_request(worker_it->second, mpayload);
                    if (!mr.has_value()) {
                        LOG_WARNING("Coordinator", "Model request from worker " +
                            worker_id + " failed: " + mr.error().message);
                    }
                    worker_it->second.profile.last_heartbeat = std::chrono::steady_clock::now();
                    scheduler_.register_device(worker_it->second.profile);
                }
            } else {
                // Drain payload for non-result messages (progress, heartbeat, etc.)
                if (hdr->payload_length > 0) {
                    std::vector<uint8_t> drain(hdr->payload_length);
                    (void)socket->receive_all(drain.data(), drain.size());                }
            }
        }

        for (const auto& wid : to_remove) {
            inflight.erase(wid);
        }

        // Avoid busy-spinning when all workers are inflight and nothing was received
        bool any_inflight = !inflight.empty();
        bool all_busy = any_inflight && pending_pool.empty();
        if (all_busy || (!any_inflight && !pending_pool.empty() && workers_.empty())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    LOG_INFO("Coordinator", "Stitching " + std::to_string(all_results.size()) + " tiles into final image");

    auto stitched = stitch_tiles(all_results, width, height, channels, scale_factor);
    if (!stitched.has_value()) {
        return std::unexpected(CoordinatorError{5, "Failed to stitch tiles: " + stitched.error().message});
    }

    return stitched;
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
