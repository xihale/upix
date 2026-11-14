// Tests for share/core/serve/daemon_control.{hpp,cpp} and ServeDaemon edge cases.
// Uses mock LocalServer instances so no GPU or model files are needed.

#include "core/distributed/protocol.hpp"
#include "core/serve/daemon.hpp"
#include "core/serve/daemon_control.hpp"
#include "utils/local_socket.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace upix;  // NOLINT

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

auto make_frame(MessageType type, const void* payload, uint32_t len)
    -> std::vector<uint8_t> {
    MessageHeader h{};
    h.type = type;
    h.payload_length = len;
    return Protocol::serialize(h, payload);
}

auto recv_serve_request(LocalSocket& sock,
                        ServeRequest& out_req,
                        std::string& out_in_path,
                        std::string& out_out_path) -> bool {
    uint8_t hbuf[Protocol::HEADER_SIZE]{};
    auto hr = sock.receive_all(hbuf, sizeof(hbuf));
    if (!hr) return false;
    auto hdr = Protocol::parse_header(hbuf);
    if (!hdr || hdr->type != MessageType::MSG_SERVE_REQUEST) return false;
    if (hdr->payload_length < sizeof(ServeRequest)) return false;

    std::vector<uint8_t> body(hdr->payload_length);
    auto br = sock.receive_all(body.data(), body.size());
    if (!br) return false;
    if (!Protocol::verify_frame(hbuf, body.data(), body.size())) {
        return false;
    }

    std::memcpy(&out_req, body.data(), sizeof(ServeRequest));
    const size_t paths_off = sizeof(ServeRequest);
    if (paths_off + out_req.input_path_length + out_req.output_path_length != body.size()) {
        return false;
    }
    out_in_path.assign(reinterpret_cast<const char*>(body.data() + paths_off),
                       out_req.input_path_length);
    out_out_path.assign(
        reinterpret_cast<const char*>(body.data() + paths_off + out_req.input_path_length),
        out_req.output_path_length);
    return true;
}

auto make_endpoint(const std::filesystem::path& dir, std::string_view name)
    -> std::string {
#ifdef _WIN32
    // Windows LocalServer expects a named-pipe name (no drive/directory separators).
    (void)dir;
    return "upix-sdc-" + std::to_string(current_pid()) + "-" + std::string(name);
#else
    return (dir / (std::string(name) + ".sock")).string();
#endif
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("upix_sdc_" + std::to_string(current_pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    CHECK(!ec);

    // 1. current_executable_path & endpoint / pid-file checks.
    {
        auto exe = current_executable_path();
        CHECK(exe.has_value());
        CHECK(fs::exists(*exe));
        CHECK(fs::is_regular_file(*exe));

        const std::string missing_sock = make_endpoint(dir, "missing");
        CHECK(!daemon_endpoint_present(missing_sock));
        auto st = serve_daemon_status(missing_sock);
        CHECK(!st.running);
        CHECK(!st.stale);

        const auto pid_path = serve_pid_file_path(missing_sock);
        fs::create_directories(fs::path(pid_path).parent_path(), ec);
        {
            std::ofstream pf(pid_path, std::ios::binary);
            pf << "garbage-pid\n";
        }
        auto stale_st = serve_daemon_status(missing_sock);
        CHECK(!stale_st.running);
        CHECK(stale_st.stale);
        CHECK(!serve_daemon_stop(missing_sock));
        fs::remove(pid_path, ec);
    }

    // 2. ServeDaemon start fails cleanly on empty models_dir and accepts idle_timeout(0/1).
    {
        const auto empty_models = dir / "empty_models";
        fs::create_directories(empty_models, ec);
        const std::string sock = make_endpoint(dir, "daemon_fail");
        ServeDaemon d;
        d.set_idle_timeout(0);
        d.set_idle_timeout(1);
        int rc = d.start(sock, empty_models.string(), "", ServeDevice::CPU);
        CHECK(rc != 0);
        CHECK(!daemon_endpoint_present(sock));
    }

    // 3. Mock LocalServer: progress frames, force flag propagation, skipped=1, out_path.
    {
        const std::string sock_path = make_endpoint(dir, "mock_ok");
        const std::string expected_in = (dir / "input.png").lexically_normal().string();
        const std::string expected_out = (dir / "output.png").lexically_normal().string();
        LocalServer server;
        auto lres = server.listen(sock_path);
        CHECK(lres.has_value());
        CHECK(daemon_endpoint_present(sock_path));

        std::thread srv([&]() {
            auto acc = server.accept();
            CHECK(acc.has_value());
            auto sock = std::move(*acc);

            ServeRequest req{};
            std::string in_p, out_p;
            CHECK(recv_serve_request(*sock, req, in_p, out_p));
            CHECK(req.force == 1);
            CHECK(in_p == expected_in);
            CHECK(out_p == expected_out);
            CHECK(std::string(req.model_name) == "test-model");

            ServeProgress p1{1, 2, 0};
            auto f1 = make_frame(MessageType::MSG_SERVE_PROGRESS, &p1, sizeof(p1));
            CHECK(sock->send_all(f1.data(), f1.size()).has_value());

            ServeProgress p2{2, 2, 1};
            auto f2 = make_frame(MessageType::MSG_SERVE_PROGRESS, &p2, sizeof(p2));
            CHECK(sock->send_all(f2.data(), f2.size()).has_value());

            const std::string resp_path = "/tmp/output_final.png";
            ServeResponse resp{};
            resp.ok = 1;
            resp.width = 128;
            resp.height = 96;
            resp.channels = 3;
            resp.out_path_length = static_cast<uint32_t>(resp_path.size());
            resp.skipped = 1;
            std::snprintf(resp.model_name, sizeof(resp.model_name), "%s", "resolved-model");

            std::vector<uint8_t> payload(sizeof(ServeResponse) + resp_path.size());
            std::memcpy(payload.data(), &resp, sizeof(ServeResponse));
            std::memcpy(payload.data() + sizeof(ServeResponse),
                        resp_path.data(), resp_path.size());
            auto rf = make_frame(MessageType::MSG_SERVE_RESPONSE,
                                 payload.data(), static_cast<uint32_t>(payload.size()));
            CHECK(sock->send_all(rf.data(), rf.size()).has_value());
        });

        ServeRequestArgs args;
        args.input_path = expected_in;
        args.output_path = expected_out;
        args.model_name = "test-model";
        args.output_format = "png";
        args.force = true;

        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> ticks;
        ServeOutcome out = serve_request(
            args, sock_path, nullptr,
            [&](uint32_t done, uint32_t total, uint32_t stage) {
                ticks.emplace_back(done, total, stage);
            });
        srv.join();
        server.stop();

        CHECK(out.ok);
        CHECK(out.exit_code == 0);
        CHECK(out.skipped);
        CHECK(!out.cancelled);
        CHECK(out.width == 128);
        CHECK(out.height == 96);
        CHECK(out.channels == 3);
        CHECK(out.out_path == "/tmp/output_final.png");
        CHECK(out.model_name == "resolved-model");
        CHECK(ticks.size() == 2);
        CHECK(std::get<2>(ticks[0]) == 0);
        CHECK(std::get<2>(ticks[1]) == 1);
    }

    // 4. Mid-request cancellation wakes blocked serve_request promptly via shutdown_wake.
    {
        const std::string sock_path = make_endpoint(dir, "mock_cancel");
        LocalServer server;
        CHECK(server.listen(sock_path).has_value());

        std::atomic<bool> stop_server{false};
        std::thread srv([&]() {
            auto acc = server.accept();
            CHECK(acc.has_value());
            auto sock = std::move(*acc);
            ServeRequest req{};
            std::string in_p, out_p;
            CHECK(recv_serve_request(*sock, req, in_p, out_p));
            ServeProgress p1{1, 10, 0};
            auto f1 = make_frame(MessageType::MSG_SERVE_PROGRESS, &p1, sizeof(p1));
            CHECK(sock->send_all(f1.data(), f1.size()).has_value());
            // Park holding socket open until client cancels and exits serve_request.
            while (!stop_server.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        ServeRequestArgs args;
        args.input_path = "/tmp/in.png";
        std::atomic<bool> cancel{false};
        const auto t0 = std::chrono::steady_clock::now();
        ServeOutcome out = serve_request(
            args, sock_path, &cancel,
            [&](uint32_t, uint32_t, uint32_t) { cancel.store(true); });
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        stop_server.store(true);
        srv.join();
        server.stop();

        CHECK(!out.ok);
        CHECK(out.cancelled);
        CHECK(elapsed < std::chrono::seconds(2));
    }

    // 5. Hostile oversized payload_length and bad checksum are rejected cleanly.
    for (int mode = 0; mode < 2; ++mode) {
        const std::string sock_path =
            make_endpoint(dir, "mock_bad_" + std::to_string(mode));
        LocalServer server;
        CHECK(server.listen(sock_path).has_value());

        std::thread srv([&]() {
            auto acc = server.accept();
            CHECK(acc.has_value());
            auto sock = std::move(*acc);
            ServeRequest req{};
            std::string in_p, out_p;
            CHECK(recv_serve_request(*sock, req, in_p, out_p));

            if (mode == 0) {
                // Header claiming 64 MiB payload -> must reject before allocating.
                uint8_t raw_hdr[Protocol::HEADER_SIZE]{};
                uint32_t magic = Protocol::MAGIC;
                uint16_t type = static_cast<uint16_t>(MessageType::MSG_SERVE_RESPONSE);
                uint32_t len = 64u * 1024u * 1024u;
                uint32_t crc = 0;
                std::memcpy(raw_hdr + 0, &magic, 4);
                std::memcpy(raw_hdr + 4, &type, 2);
                std::memcpy(raw_hdr + 6, &len, 4);
                std::memcpy(raw_hdr + 10, &crc, 4);
                (void)sock->send_all(raw_hdr, sizeof(raw_hdr));
            } else {
                // Valid size, corrupted payload byte so checksum fails.
                ServeResponse resp{};
                resp.ok = 1;
                auto frame = make_frame(MessageType::MSG_SERVE_RESPONSE,
                                        &resp, sizeof(resp));
                frame.back() ^= 0xFF;
                (void)sock->send_all(frame.data(), frame.size());
            }
        });

        ServeRequestArgs args;
        args.input_path = "/tmp/bad.png";
        ServeOutcome out = serve_request(args, sock_path);
        srv.join();
        server.stop();

        CHECK(!out.ok);
        CHECK(out.exit_code != 0);
    }

    // 6. Legacy ServeResponse (sizeof - 8, no skipped/reserved0 tail) compatibility.
    {
        const std::string sock_path = make_endpoint(dir, "mock_legacy");
        LocalServer server;
        CHECK(server.listen(sock_path).has_value());

        std::thread srv([&]() {
            auto acc = server.accept();
            CHECK(acc.has_value());
            auto sock = std::move(*acc);
            ServeRequest req{};
            std::string in_p, out_p;
            CHECK(recv_serve_request(*sock, req, in_p, out_p));

            const std::string legacy_path = "/tmp/legacy.png";
            ServeResponse resp{};
            resp.ok = 1;
            resp.width = 64;
            resp.height = 64;
            resp.channels = 4;
            resp.out_path_length = static_cast<uint32_t>(legacy_path.size());
            constexpr size_t kLegacySize = sizeof(ServeResponse) - 8;
            std::vector<uint8_t> payload(kLegacySize + legacy_path.size());
            std::memcpy(payload.data(), &resp, kLegacySize);
            std::memcpy(payload.data() + kLegacySize,
                        legacy_path.data(), legacy_path.size());
            auto frame = make_frame(MessageType::MSG_SERVE_RESPONSE,
                                    payload.data(), static_cast<uint32_t>(payload.size()));
            CHECK(sock->send_all(frame.data(), frame.size()).has_value());
        });

        ServeRequestArgs args;
        args.input_path = "/tmp/legacy_in.png";
        ServeOutcome out = serve_request(args, sock_path);
        srv.join();
        server.stop();

        CHECK(out.ok);
        CHECK(!out.skipped);
        CHECK(out.out_path == "/tmp/legacy.png");
    }

    fs::remove_all(dir, ec);
    std::printf("ALL SERVE DAEMON CONTROL TESTS PASSED\n");
    return 0;
}
