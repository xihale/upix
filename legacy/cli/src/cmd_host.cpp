// `upix host {start, info}` — the coordinator phones pair with.
//
// start: launch a Coordinator on `port`, print the connection card (IP/QR/key),
//        and poll until Ctrl+C. Each submitted image is upscaled on the PC's own
//        engine (GPU, CPU fallback). Auto-generates an identity key if none.
// info:  just print the connection card for the given port/key (no server).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>

#include "args.hpp"
#include "cli_app.hpp"
#include "common.hpp"
#include "utils/logger.hpp"
#include "core/distributed/coordinator.hpp"
#include "core/distributed/identity.hpp"

int cmd_host(const args::Host& a) {
    // Machine event mode before the first LOG_* so stdout stays parseable.
    if (a.json) enable_machine_output();

    // Card document: one-shot for `info --json`, "t":"card" event for start.
    auto emit_card = [](const ConnectionCard& card, bool event) {
        std::string ips;
        for (size_t i = 0; i < card.addresses.size(); ++i) {
            if (i > 0) ips += ",";
            ips += std::format(R"("{}")", json_escape(card.addresses[i]));
        }
        const std::string head = event ? R"({"t":"card",)" : "{";
        emit_json_line(std::format(
            R"({}"ips":[{}],"port":{},"fingerprint":"{}","qr":"{}"}})",
            head, ips, card.port, json_escape(card.fingerprint),
            json_escape(card.qr_payload)));
    };

    if (a.subcommand == "info") {
        auto identity = upix::IdentityKey::load_or_create(
            upix::default_identity_path());
        if (a.json) {
            emit_card(build_connection_card(a.port, identity.public_key()),
                      false);
        } else {
            print_connection_card(a.port, identity.public_key());
        }
        return 0;
    }

    // ── start ───────────────────────────────────────────────────────────────
    auto identity = upix::IdentityKey::load_or_create(
        upix::default_identity_path());
    LOG_INFO("Host", "PC identity fingerprint: {}",
             upix::fingerprint(identity.public_key()));

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto coordinator = std::make_unique<upix::Coordinator>();
    coordinator->set_models_directory(a.models_dir);
    coordinator->set_trust_store_path(upix::default_trust_path());
    const bool machine = a.json;
    coordinator->on_task_progress() = [machine](float progress) {
        static std::atomic<int> last_pct{-1};
        const auto pct = static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f);
        const int previous = last_pct.exchange(pct);
        if (previous == pct) {
            return;
        }
        if (machine) {
            emit_json_line(
                std::format(R"({{"t":"task","pct":{}}})", pct));
        } else {
            LOG_INFO("Host", "Task progress: {}%", pct);
        }
    };

    auto start_result = coordinator->start(a.port, identity);
    if (!start_result.has_value()) {
        LOG_ERROR("Host", "Failed to start coordinator: {}", start_result.error().message);
        return 1;
    }

    LOG_INFO("Host", "Coordinator running. Press Ctrl+C to stop.");

    const auto card = build_connection_card(a.port, identity.public_key());
    if (machine) {
        emit_card(card, true);
    } else {
        print_connection_card(a.port, identity.public_key());
    }

    while (g_running && coordinator->is_running()) {
        // 服务待批准的配对请求(仅首次连接的手机;已信任的自动通过)。
        // --auto-approve 跳过 stdin [y/n] 提示,直接批准 — 供自动化测试/CI。
        // 否则分片读 stdin(见 read_line_interruptible):Ctrl+C/SIGTERM 能在
        // 一个 tick 内结束等待。--json 下 GUI 消费 pairing 事件,并向 stdin
        // 写入同一条 "y\n"/"n\n" wire。
        if (auto req = coordinator->pop_pending_approval()) {
            bool approved = false;
            if (machine) {
                emit_json_line(std::format(
                    R"({{"t":"pairing","device":"{}","fingerprint":"{}"}})",
                    json_escape(req->device_name),
                    json_escape(req->fingerprint)));
            }
            // The prompt must stay interruptible: std::signal() arms its handler
            // with SA_RESTART, so a blocked std::getline() is restarted instead
            // of returning — Ctrl+C at the prompt looked like a hang until the
            // user pressed Enter. read_line_interruptible() polls in 200 ms ticks
            // and re-checks g_running / the coordinator's state, so a signal
            // denies the pending request and lets the loop exit. The --json wire
            // (GUI writes "y\n"/"n\n") and the y/n semantics are unchanged.
            const auto still_waiting = [&] {
                return g_running.load() && coordinator->is_running();
            };
            const auto is_yes = [](const std::string& line) {
                return line == "y" || line == "Y" || line == "yes" || line == "YES";
            };
            if (a.auto_approve) {
                approved = true;
                LOG_INFO("Host", "自动批准配对(--auto-approve): {} ({})",
                         req->device_name, req->fingerprint);
            } else if (machine) {
                std::string line;
                if (read_line_interruptible(line, still_waiting)) {
                    approved = is_yes(line);
                }
                // stdin EOF(例如后台运行)或停机 — 默认拒绝,避免无限等待。
            } else {
                std::print("\n┌─ 配对请求 ───────────────────────────────────────\n"
                           "│ 设备: {}\n"
                           "│ 指纹: {}\n"
                           "\n└──────────────────────────────────────────────────┘\n"
                           "允许此设备连接? [y/n]: ",
                           req->device_name, req->fingerprint);
                std::fflush(stdout);
                std::string line;
                if (read_line_interruptible(line, still_waiting)) {
                    approved = is_yes(line);
                } else {
                    // stdin EOF(例如后台运行)或收到停机信号 — 默认拒绝。
                    std::println("(stdin 不可读或已中断,默认拒绝)");
                }
            }
            coordinator->resolve_approval(req->id, approved);
            if (approved) {
                LOG_INFO("Host", "已批准配对: {} ({}) — 已加入 TrustStore",
                         req->device_name, req->fingerprint);
            } else {
                LOG_WARNING("Host", "已拒绝配对: {}", req->device_name);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("Host", "Shutting down coordinator...");
    coordinator->stop();
    return 0;
}
