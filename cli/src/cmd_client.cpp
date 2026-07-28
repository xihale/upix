// `crossscale client process` — one-shot client that ships an image to a
// running coordinator for remote upscale and writes the result to disk.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/image.hpp"
#include "utils/logger.hpp"
#include "utils/network.hpp"
#include "core/distributed/protocol.hpp"

namespace {

auto split_host_port(const std::string& addr, std::string& host, uint16_t& port) -> bool {
    auto colon = addr.find(':');
    if (colon == std::string::npos) {
        LOG_ERROR("Client", "Invalid server address format. Use <host>:<port>");
        return false;
    }
    host = addr.substr(0, colon);
    try {
        long p = std::stol(addr.substr(colon + 1));
        if (p < 1 || p > 65535) throw std::out_of_range("port");
        port = static_cast<uint16_t>(p);
    } catch (const std::exception&) {
        LOG_ERROR("Client", "Invalid port in server address: " + addr);
        return false;
    }
    return true;
}

}  // namespace

int cmd_client(const args::Client& a) {
    std::string host;
    uint16_t port = 0;
    if (!split_host_port(a.server_addr, host, port)) return 1;

    LOG_INFO("Client", "Loading image: " + a.input_path);
    auto image_result = crossscale::Image::load(a.input_path);
    if (!image_result.has_value()) {
        LOG_ERROR("Client", "Failed to load image: " + image_result.error().message);
        return 1;
    }
    auto& image = image_result.value();

    LOG_INFO("Client", "Connecting to coordinator at " + host + ":" + std::to_string(port));

    crossscale::TCPSocket socket;
    auto conn_result = socket.connect(host, port);
    if (!conn_result.has_value()) {
        LOG_ERROR("Client", "Failed to connect: " + conn_result.error().message);
        return 1;
    }

    // Authenticate
    crossscale::AuthRequest auth_req{};
    std::copy_n(a.auth_key.c_str(), std::min(a.auth_key.size(), sizeof(auth_req.auth_key) - 1), auth_req.auth_key);
    auth_req.auth_key[sizeof(auth_req.auth_key) - 1] = '\0';

    crossscale::MessageHeader auth_header{};
    auth_header.type = crossscale::MessageType::MSG_AUTH_REQUEST;
    auth_header.payload_length = sizeof(auth_req);

    auto auth_data = crossscale::Protocol::serialize(auth_header, &auth_req);
    // fire-and-forget: the receive_all below detects any send-side failure.
    (void)socket.send_all(auth_data.data(), auth_data.size());

    std::vector<uint8_t> resp_header_buf(crossscale::Protocol::HEADER_SIZE);
    auto auth_header_recv = socket.receive_all(resp_header_buf.data(), resp_header_buf.size());
    if (!auth_header_recv.has_value()) {
        LOG_ERROR("Client", "Failed to receive auth response header: " + auth_header_recv.error().message);
        return 1;
    }
    auto resp_header = crossscale::Protocol::parse_header(resp_header_buf.data(), resp_header_buf.size());

    if (!resp_header || resp_header->type != crossscale::MessageType::MSG_AUTH_RESPONSE) {
        LOG_ERROR("Client", "Invalid auth response");
        return 1;
    }

    if (resp_header->payload_length > MAX_NETWORK_PAYLOAD) {
        LOG_ERROR("Client", "Auth response payload too large: " + std::to_string(resp_header->payload_length));
        return 1;
    }
    std::vector<uint8_t> resp_payload(resp_header->payload_length);
    auto auth_payload_recv = socket.receive_all(resp_payload.data(), resp_payload.size());
    if (!auth_payload_recv.has_value()) {
        LOG_ERROR("Client", "Failed to receive auth response payload: " + auth_payload_recv.error().message);
        return 1;
    }
    crossscale::AuthResponse* auth_resp = reinterpret_cast<crossscale::AuthResponse*>(resp_payload.data());

    if (!auth_resp->authenticated) {
        LOG_ERROR("Client", "Authentication failed");
        return 1;
    }

    LOG_INFO("Client", "Authenticated, submitting task...");

    // Submit task
    crossscale::ClientTaskSubmit task_submit{};
    task_submit.image_width = static_cast<uint32_t>(image.width());
    task_submit.image_height = static_cast<uint32_t>(image.height());
    task_submit.channels = static_cast<uint8_t>(image.channels());
    constexpr std::string_view default_model = "Real-ESRGANv3-anime-x2";
    std::copy_n(default_model.begin(), std::min(default_model.size(), sizeof(task_submit.model_name) - 1), task_submit.model_name);
    task_submit.model_name[sizeof(task_submit.model_name) - 1] = '\0';
    task_submit.tile_size = static_cast<uint32_t>(a.tile_size);
    task_submit.tile_overlap = static_cast<uint32_t>(a.tile_overlap);
    task_submit.image_data_length = static_cast<uint32_t>(image.size());

    crossscale::MessageHeader submit_header{};
    submit_header.type = crossscale::MessageType::MSG_CLIENT_SUBMIT;
    submit_header.payload_length = sizeof(task_submit);

    auto submit_header_data = crossscale::Protocol::serialize(submit_header, &task_submit);
    // fire-and-forget: the receive_all below detects any send-side failure.
    (void)socket.send_all(submit_header_data.data(), submit_header_data.size());
    (void)socket.send_all(image.data(), image.size());

    // Receive result
    std::vector<uint8_t> result_header_buf(crossscale::Protocol::HEADER_SIZE);
    auto result_header_recv = socket.receive_all(result_header_buf.data(), result_header_buf.size());
    if (!result_header_recv.has_value()) {
        LOG_ERROR("Client", "Failed to receive task response header: " + result_header_recv.error().message);
        return 1;
    }
    auto result_header = crossscale::Protocol::parse_header(result_header_buf.data(), result_header_buf.size());

    if (!result_header || result_header->type != crossscale::MessageType::MSG_CLIENT_RESPONSE) {
        LOG_ERROR("Client", "Invalid task response header");
        return 1;
    }

    if (result_header->payload_length > MAX_NETWORK_PAYLOAD) {
        LOG_ERROR("Client", "Task response payload too large: " + std::to_string(result_header->payload_length));
        return 1;
    }
    std::vector<uint8_t> result_resp_payload(result_header->payload_length);
    auto result_payload_recv = socket.receive_all(result_resp_payload.data(), result_resp_payload.size());
    if (!result_payload_recv.has_value()) {
        LOG_ERROR("Client", "Failed to receive task response payload: " + result_payload_recv.error().message);
        return 1;
    }
    crossscale::ClientTaskResponse* task_resp = reinterpret_cast<crossscale::ClientTaskResponse*>(result_resp_payload.data());

    if (!task_resp->success) {
        LOG_ERROR("Client", "Task failed: " + std::string(task_resp->error_message));
        return 1;
    }

    LOG_INFO("Client", "Task completed, receiving result (" + std::to_string(task_resp->result_data_length) + " bytes)...");

    if (task_resp->result_data_length > MAX_NETWORK_PAYLOAD) {
        LOG_ERROR("Client", "Result data too large: " + std::to_string(task_resp->result_data_length));
        return 1;
    }
    std::vector<uint8_t> result_data(task_resp->result_data_length);
    auto result_data_recv = socket.receive_all(result_data.data(), result_data.size());
    if (!result_data_recv.has_value()) {
        LOG_ERROR("Client", "Failed to receive task result bytes: " + result_data_recv.error().message);
        return 1;
    }

    std::ofstream out_file(a.output_path, std::ios::binary);
    if (!out_file.is_open()) {
        LOG_ERROR("Client", "Failed to open output file: " + a.output_path);
        return 1;
    }
    out_file.write(reinterpret_cast<const char*>(result_data.data()), result_data.size());
    out_file.close();

    if (!out_file) {
        LOG_ERROR("Client", "Failed to write output file: " + a.output_path);
        return 1;
    }

    LOG_INFO("Client", "Result saved to: " + a.output_path);
    return 0;
}
