// Local-endpoint transport tests: AF_UNIX on POSIX, named pipe on Windows
// (legacy/share/utils/local_socket.hpp). These are the transport contract the serve
// daemon/client rely on — runnable headlessly, and buildable as a WIN32
// binary (mingw/wine) to pin the Windows backend without a GUI.
#include "utils/local_socket.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
static int current_pid() { return _getpid(); }
#else
#include <unistd.h>
static int current_pid() { return static_cast<int>(::getpid()); }
#endif

using namespace upix;

namespace {
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

auto endpoint_for(const char* stem) -> std::string {
    // Unique per process: pipes share a machine-global namespace, and POSIX
    // tests may run in parallel from different worktrees.
    return std::string("upix-test-") + stem + "-" + std::to_string(current_pid());
}
}  // namespace

int main() {
    // 1. Echo roundtrip: 256 KB through the framing layer both ways.
    {
        const std::string ep = endpoint_for("echo");
        LocalServer server;
        auto listen_res = server.listen(ep);
        check(listen_res.has_value(), "listen on a fresh endpoint");
        if (!listen_res) return 1;

        std::thread srv([&] {
            auto client = server.accept();
            if (!client) return;
            std::vector<uint8_t> buf(256 * 1024);
            if (auto r = (*client)->receive_all(buf.data(), buf.size()); !r) return;
            (void)(*client)->send_all(buf.data(), buf.size());
            // Second request: shutdown_wake contract check happens in test 4.
        });

        LocalSocket sock;
        auto conn = sock.connect(ep);
        check(conn.has_value(), "connect to the listening endpoint");

        std::vector<uint8_t> payload(256 * 1024);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>(i * 31 + 7);
        }
        check(sock.send_all(payload.data(), payload.size()).has_value(),
              "send 256 KB");
        std::vector<uint8_t> echo(payload.size());
        const bool echoed = sock.receive_all(echo.data(), echo.size()).has_value();
        check(echoed && std::memcmp(echo.data(), payload.data(), payload.size()) == 0,
              "echo roundtrip is byte-identical");
        sock.close();

        // 2. Live-server guard: a second listen on the same name must fail
        //    (POSIX bind-probe, Windows FILE_FLAG_FIRST_PIPE_INSTANCE).
        LocalServer second;
        check(!second.listen(ep).has_value(), "second listen on the same endpoint fails");

        srv.join();
        server.stop();

        // 3. After stop(), connecting reports "no daemon".
        LocalSocket late;
        check(!late.connect(ep).has_value(), "connect after stop fails (no daemon)");

        // 3b. Connecting to a never-created endpoint fails too.
        LocalSocket never;
        check(!never.connect(endpoint_for("never")).has_value(),
              "connect to a missing endpoint fails");
    }

    // 4. shutdown_wake must interrupt a peer thread blocked in receive_all.
    {
        const std::string ep = endpoint_for("wake");
        LocalServer server;
        check(server.listen(ep).has_value(), "listen for the wake test");
        std::atomic<bool> interrupted{false};
        std::atomic<LocalSocket*> conn_ptr{nullptr};

        std::thread srv([&] {
            auto client = server.accept();
            if (!client) return;
            conn_ptr.store(client->get());
            uint8_t one = 0;
            auto r = (*client)->receive_all(&one, 1);  // blocks: nothing sent
            interrupted = !r.has_value();
        });

        LocalSocket sock;
        check(sock.connect(ep).has_value(), "client connects for the wake test");
        // Wait for the server-side accept to land.
        for (int i = 0; i < 200 && conn_ptr.load() == nullptr; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sock.shutdown_wake();  // peer's blocked receive must fail now
        srv.join();
        check(interrupted.load(), "shutdown_wake interrupts a blocked receive");
        server.stop();
    }

    // 5. accept() ticks: no client → EAGAIN loop exit, then a client works.
    {
        const std::string ep = endpoint_for("tick");
        LocalServer server;
        check(server.listen(ep).has_value(), "listen for the tick test");
        int ticks = 0;
        std::unique_ptr<LocalSocket> accepted;
        std::thread client_thread;
        for (int i = 0; i < 200 && !accepted; ++i) {
            auto c = server.accept();
            if (!c) {
                ++ticks;
                if (ticks == 2 && !client_thread.joinable()) {
                    // Second timeout: dial from a client thread.
                    client_thread = std::thread([&] {
                        LocalSocket s;
                        (void)s.connect(ep);
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    });
                }
                continue;
            }
            accepted = std::move(*c);
        }
        check(accepted != nullptr, "accept yields a client after timeouts");
        check(ticks >= 1, "accept times out (EAGAIN tick) when idle");
        if (client_thread.joinable()) client_thread.join();
        server.stop();
    }

    // 6. watch_dup/watch_kill interrupts a serving thread's blocked read —
    //    the daemon teardown helper.
    {
        const std::string ep = endpoint_for("watch");
        LocalServer server;
        check(server.listen(ep).has_value(), "listen for the watch test");
        std::atomic<bool> done{false};
        std::atomic<intptr_t> watch{-1};
        std::thread srv([&] {
            auto client = server.accept();
            if (!client) return;
            // The daemon's pattern: dup the connection handle into the thread
            // record so teardown can interrupt the blocked read.
            watch.store(watch_dup((*client)->handle()));
            uint8_t one = 0;
            auto r = (*client)->receive_all(&one, 1);
            done = !r.has_value();
        });
        LocalSocket sock;
        check(sock.connect(ep).has_value(), "client connects for the watch test");
        for (int i = 0; i < 200 && watch.load() < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        watch_kill(watch.load());  // teardown: interrupt the blocked read
        srv.join();
        check(done.load(), "watch_kill interrupts the serving read");
        server.stop();
    }

    std::printf("RESULT: %s (%d failures)\n", g_failures ? "FAIL" : "PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
