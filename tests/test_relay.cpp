// TCP relay regression and performance runner.
//
// Build (VS2022 dev prompt):
//   cl /EHsc /std:c++latest /utf-8 /D_WIN32_WINNT=0x0A00 /DNOMINMAX ^
//      /I../src /I"%VCPKG_ROOT%/installed/x64-windows/include" test_relay.cpp ^
//      /link /LIBPATH:"%VCPKG_ROOT%/installed/x64-windows/lib" ws2_32.lib fmt.lib psapi.lib
//
// No administrator rights, WinDivert, external proxy, or external network needed.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

#include "proxy/acceptor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void throw_if(const std::error_code& ec, const std::string& operation) {
    if (ec) throw std::runtime_error(operation + ": " + ec.message());
}

struct FailureLog {
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::string first_error;

    void add(const std::string& error) {
        failed.store(true, std::memory_order_release);
        std::lock_guard lock(mutex);
        if (first_error.empty()) first_error = error;
    }

    void check() const {
        require(!failed.load(std::memory_order_acquire), first_error);
    }
};

struct ConnectedClient {
    tcp::socket socket;
    uint16_t port;
};

class RelayFixture {
public:
    enum class Mode { abrupt_close, echo, client_eof, client_rst, proxy_eof, proxy_rst, handshake_eof };

    RelayFixture(size_t worker_count, Mode mode)
        : work_(asio::make_work_guard(ioc_))
        , tracker_(std::make_unique<clew::PortTracker>())
        , shared_strand_(asio::make_strand(ioc_))
        , proxy_acceptor_(ioc_, tcp::endpoint(asio::ip::address_v4::loopback(), 0))
        , groups_{{0, {"127.0.0.1", proxy_acceptor_.local_endpoint().port()}}}
        , acceptor_(ioc_, *tracker_, shared_strand_, groups_)
        , mode_(mode) {
        acceptor_.start();
        asio::co_spawn(ioc_, proxy_accept_loop(), asio::detached);
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { ioc_.run(); });
        }
    }

    ~RelayFixture() {
        shutdown();
    }

    RelayFixture(const RelayFixture&) = delete;
    RelayFixture& operator=(const RelayFixture&) = delete;

    ConnectedClient connect_client(int64_t timestamp) {
        ConnectedClient client{tcp::socket(client_ioc_), 0};
        std::error_code ec;
        client.socket.open(tcp::v4(), ec);
        throw_if(ec, "client open");
        client.socket.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
        throw_if(ec, "client bind");
        client.port = client.socket.local_endpoint().port();

        clew::TrackerEntry entry{};
        entry.remote_addr[0] = 0x01020304;
        entry.remote_port = 80;
        entry.group_id = 0;
        tracker_->publish(client.port, clew::slot_state::proxied, entry, timestamp);

        client.socket.connect(
            tcp::endpoint(asio::ip::address_v4::loopback(), acceptor_.port()), ec);
        throw_if(ec, "client connect");
        return client;
    }

    void exchange_byte(ConnectedClient& client) {
        unsigned char sent = 0x5a;
        unsigned char received = 0;
        asio::write(client.socket, asio::buffer(&sent, 1));
        asio::read(client.socket, asio::buffer(&received, 1));
        require(received == sent, "relay byte echo mismatch");
    }

    void close_client(ConnectedClient& client, bool abrupt) {
        std::error_code ec;
        if (abrupt) {
            client.socket.set_option(asio::socket_base::linger(true, 0), ec);
        }
        client.socket.close(ec);
    }

    void require_cleanup(uint16_t port) {
        const bool cleaned = wait_until([this, port] {
            return tracker_->state(port) == clew::slot_state::empty;
        });
        require(cleaned, "relay cleanup timed out for source port " + std::to_string(port));
    }

    void require_ready(uint64_t accepted_delta, uint64_t ready_delta) {
        const auto accepted_target = accepted_base_ + accepted_delta;
        const auto ready_target = ready_base_ + ready_delta;
        const bool ready = wait_until([this, accepted_target, ready_target] {
            return acceptor_.accepted_count() >= accepted_target &&
                   proxy_ready_.load(std::memory_order_acquire) >= ready_target;
        }, 30s);
        require(ready, "relay readiness timed out");
    }

    void mark_ready_baseline() {
        accepted_base_ = acceptor_.accepted_count();
        ready_base_ = proxy_ready_.load(std::memory_order_acquire);
    }

    void stop() {
        shutdown();
    }

private:
    template <typename Predicate>
    bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
        const auto deadline = Clock::now() + timeout;
        while (!predicate()) {
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    void shutdown() {
        if (shutdown_.exchange(true)) return;
        asio::post(ioc_, [this] {
            acceptor_.stop();
            std::error_code ec;
            proxy_acceptor_.close(ec);
            ioc_.stop();
        });
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    asio::awaitable<void> proxy_accept_loop() {
        for (;;) {
            auto [ec, socket] = co_await proxy_acceptor_.async_accept(
                asio::as_tuple(asio::use_awaitable));
            if (ec) co_return;
            asio::co_spawn(ioc_, proxy_session(std::move(socket)), asio::detached);
        }
    }

    asio::awaitable<bool> proxy_handshake(tcp::socket& socket) {
        std::array<unsigned char, 3> greeting{};
        auto [read_greeting_ec, greeting_size] = co_await asio::async_read(
            socket, asio::buffer(greeting), asio::as_tuple(asio::use_awaitable));
        if (read_greeting_ec) co_return false;
        if (mode_ == Mode::handshake_eof) {
            std::error_code ec;
            socket.close(ec);
            co_return false;
        }

        const std::array<unsigned char, 2> greeting_reply{0x05, 0x00};
        auto [write_greeting_ec, greeting_reply_size] = co_await asio::async_write(
            socket, asio::buffer(greeting_reply), asio::as_tuple(asio::use_awaitable));
        if (write_greeting_ec) co_return false;

        std::array<unsigned char, 10> request{};
        auto [read_request_ec, request_size] = co_await asio::async_read(
            socket, asio::buffer(request), asio::as_tuple(asio::use_awaitable));
        if (read_request_ec) co_return false;

        const std::array<unsigned char, 10> reply{
            0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
        auto [write_reply_ec, reply_size] = co_await asio::async_write(
            socket, asio::buffer(reply), asio::as_tuple(asio::use_awaitable));
        co_return !write_reply_ec;
    }

    asio::awaitable<void> proxy_session(tcp::socket socket) {
        if (!co_await proxy_handshake(socket)) co_return;
        proxy_ready_.fetch_add(1, std::memory_order_release);

        if (mode_ != Mode::echo) {
            unsigned char request = 0;
            auto [read_ec, read_size] = co_await socket.async_read_some(
                asio::buffer(&request, 1), asio::as_tuple(asio::use_awaitable));
            if (read_ec) co_return;
            auto [write_ec, write_size] = co_await asio::async_write(
                socket, asio::buffer(&request, 1), asio::as_tuple(asio::use_awaitable));
            if (write_ec) co_return;

            if (mode_ == Mode::proxy_eof || mode_ == Mode::proxy_rst) {
                unsigned char ack = 0;
                auto [ack_ec, ack_size] = co_await socket.async_read_some(
                    asio::buffer(&ack, 1), asio::as_tuple(asio::use_awaitable));
                if (ack_ec) co_return;
            }

            std::error_code ec;
            if (mode_ != Mode::client_rst && mode_ != Mode::proxy_rst) {
                socket.shutdown(tcp::socket::shutdown_send, ec);
            }
            if (mode_ != Mode::proxy_eof && mode_ != Mode::proxy_rst) {
                unsigned char ignored = 0;
                auto [close_ec, close_size] = co_await socket.async_read_some(
                    asio::buffer(&ignored, 1), asio::as_tuple(asio::use_awaitable));
            }
            if (mode_ == Mode::abrupt_close || mode_ == Mode::proxy_rst) {
                socket.set_option(asio::socket_base::linger(true, 0), ec);
            }
            socket.close(ec);
            co_return;
        }

        co_await clew::pipe(socket, socket);
        std::error_code ec;
        socket.close(ec);
    }

    asio::io_context ioc_;
    asio::io_context client_ioc_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::unique_ptr<clew::PortTracker> tracker_;
    asio::strand<asio::io_context::executor_type> shared_strand_;
    tcp::acceptor proxy_acceptor_;
    std::unordered_map<uint32_t, clew::ProxyGroupConfig> groups_;
    clew::async_acceptor acceptor_;
    Mode mode_;
    std::atomic<uint64_t> proxy_ready_{0};
    uint64_t accepted_base_{0};
    uint64_t ready_base_{0};
    std::atomic<bool> shutdown_{false};
    std::vector<std::thread> workers_;
};

void run_abrupt_close_stress(size_t iterations, size_t concurrency, size_t worker_count) {
    RelayFixture fixture(worker_count, RelayFixture::Mode::abrupt_close);
    FailureLog failures;
    std::atomic<size_t> next{0};
    std::atomic<size_t> completed{0};
    std::vector<std::thread> clients;
    clients.reserve(concurrency);
    const auto started = Clock::now();

    for (size_t thread_index = 0; thread_index < concurrency; ++thread_index) {
        clients.emplace_back([&] {
            try {
                while (!failures.failed.load(std::memory_order_acquire)) {
                    const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= iterations) break;
                    auto client = fixture.connect_client(static_cast<int64_t>(index + 1));
                    fixture.exchange_byte(client);
                    fixture.close_client(client, true);
                    fixture.require_cleanup(client.port);
                    completed.fetch_add(1, std::memory_order_release);
                }
            } catch (const std::exception& e) {
                failures.add(e.what());
            }
        });
    }
    for (auto& client : clients) client.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    failures.check();
    require(completed.load() == iterations, "not all abrupt-close iterations completed");

    std::cout << std::fixed << std::setprecision(2)
              << "PASS abrupt_close: connections=" << iterations
              << " concurrency=" << concurrency
              << " workers=" << worker_count
              << " seconds=" << seconds
              << " connections_per_second=" << (iterations / seconds) << '\n';
}

void set_native_timeouts(SOCKET socket) {
    const DWORD timeout_ms = 10000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
}

std::string_view mode_name(RelayFixture::Mode mode) {
    switch (mode) {
        case RelayFixture::Mode::client_eof: return "client_eof";
        case RelayFixture::Mode::client_rst: return "client_rst";
        case RelayFixture::Mode::proxy_eof: return "proxy_eof";
        case RelayFixture::Mode::proxy_rst: return "proxy_rst";
        case RelayFixture::Mode::handshake_eof: return "handshake_eof";
        default: return "unknown";
    }
}

void run_teardown_case(RelayFixture::Mode mode, size_t iterations, size_t worker_count) {
    RelayFixture fixture(worker_count, mode);
    FailureLog failures;
    std::atomic<size_t> next{0};
    std::atomic<size_t> completed{0};
    const size_t concurrency = std::min<size_t>(16, iterations);
    std::vector<std::thread> clients;
    clients.reserve(concurrency);

    for (size_t thread_index = 0; thread_index < concurrency; ++thread_index) {
        clients.emplace_back([&] {
            try {
                while (!failures.failed.load(std::memory_order_acquire)) {
                    const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= iterations) break;
                    auto client = fixture.connect_client(static_cast<int64_t>(index + 1));
                    set_native_timeouts(client.socket.native_handle());

                    if (mode == RelayFixture::Mode::handshake_eof) {
                        fixture.require_cleanup(client.port);
                        fixture.close_client(client, false);
                        completed.fetch_add(1, std::memory_order_release);
                        continue;
                    }

                    fixture.exchange_byte(client);
                    std::error_code ec;
                    if (mode == RelayFixture::Mode::client_eof) {
                        client.socket.shutdown(tcp::socket::shutdown_send, ec);
                        throw_if(ec, "client shutdown");
                    }
                    if (mode == RelayFixture::Mode::client_eof ||
                        mode == RelayFixture::Mode::proxy_eof ||
                        mode == RelayFixture::Mode::proxy_rst) {
                        if (mode != RelayFixture::Mode::client_eof) {
                            const unsigned char ack = 0xa5;
                            asio::write(client.socket, asio::buffer(&ack, 1), ec);
                            throw_if(ec, "client ack");
                        }
                        unsigned char ignored = 0;
                        client.socket.read_some(asio::buffer(&ignored, 1), ec);
                        require(static_cast<bool>(ec), "teardown did not reach the client");
                    }
                    fixture.close_client(client, mode == RelayFixture::Mode::client_rst);
                    fixture.require_cleanup(client.port);
                    completed.fetch_add(1, std::memory_order_release);
                }
            } catch (const std::exception& e) {
                failures.add(e.what());
            }
        });
    }
    for (auto& client : clients) client.join();
    failures.check();
    require(completed.load() == iterations, "not all directional teardown cases completed");
    std::cout << std::unitbuf << "PASS directional_teardown: mode=" << mode_name(mode)
              << " connections=" << iterations << '\n';
}

void run_teardown_matrix(size_t iterations, size_t worker_count) {
    const RelayFixture::Mode modes[] = {
        RelayFixture::Mode::client_eof,
        RelayFixture::Mode::client_rst,
        RelayFixture::Mode::proxy_eof,
        RelayFixture::Mode::proxy_rst,
        RelayFixture::Mode::handshake_eof,
    };
    for (const auto mode : modes) run_teardown_case(mode, iterations, worker_count);
}

void run_echo_transfer(ConnectedClient& client, size_t total_bytes,
                       std::atomic<bool>& io_failed) {
    const SOCKET native = client.socket.native_handle();
    set_native_timeouts(native);
    std::vector<char> send_buffer(32 * 1024, 'x');

    std::thread reader([&] {
        size_t received = 0;
        std::vector<char> receive_buffer(32 * 1024);
        while (received < total_bytes && !io_failed.load(std::memory_order_acquire)) {
            const int result = ::recv(native, receive_buffer.data(),
                static_cast<int>(receive_buffer.size()), 0);
            if (result <= 0) {
                io_failed.store(true, std::memory_order_release);
                ::shutdown(native, SD_BOTH);
                break;
            }
            received += static_cast<size_t>(result);
        }
    });

    size_t sent = 0;
    while (sent < total_bytes && !io_failed.load(std::memory_order_acquire)) {
        const int result = ::send(native, send_buffer.data(),
            static_cast<int>(send_buffer.size()), 0);
        if (result <= 0) {
            io_failed.store(true, std::memory_order_release);
            ::shutdown(native, SD_BOTH);
            break;
        }
        sent += static_cast<size_t>(result);
    }
    reader.join();
    require(!io_failed.load(std::memory_order_acquire), "echo transfer failed");
}

void run_throughput(size_t connections, size_t bytes_per_connection, size_t worker_count) {
    RelayFixture fixture(worker_count, RelayFixture::Mode::echo);
    FailureLog failures;
    std::atomic<size_t> ready{0};
    std::atomic<size_t> finished{0};
    std::atomic<bool> start{false};
    std::atomic<bool> cleanup{false};
    std::vector<std::thread> clients;
    clients.reserve(connections);

    for (size_t index = 0; index < connections; ++index) {
        clients.emplace_back([&, index] {
            bool announced_ready = false;
            bool announced_finished = false;
            try {
                auto client = fixture.connect_client(static_cast<int64_t>(index + 1));
                ready.fetch_add(1, std::memory_order_release);
                announced_ready = true;
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                std::atomic<bool> io_failed{false};
                run_echo_transfer(client, bytes_per_connection, io_failed);
                finished.fetch_add(1, std::memory_order_release);
                announced_finished = true;
                while (!cleanup.load(std::memory_order_acquire)) std::this_thread::yield();
                fixture.close_client(client, false);
                fixture.require_cleanup(client.port);
            } catch (const std::exception& e) {
                failures.add(e.what());
                if (!announced_ready) ready.fetch_add(1, std::memory_order_release);
                if (!announced_finished) finished.fetch_add(1, std::memory_order_release);
            }
        });
    }

    const auto ready_deadline = Clock::now() + 30s;
    while (ready.load(std::memory_order_acquire) != connections) {
        require(Clock::now() < ready_deadline, "throughput clients failed to connect");
        std::this_thread::sleep_for(1ms);
    }
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    const auto finish_deadline = Clock::now() + 120s;
    while (finished.load(std::memory_order_acquire) != connections) {
        require(Clock::now() < finish_deadline, "throughput transfer timed out");
        std::this_thread::sleep_for(1ms);
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const double mib = static_cast<double>(connections * bytes_per_connection) / (1024.0 * 1024.0);
    std::cout << std::unitbuf << std::fixed << std::setprecision(2)
              << "MEASURE throughput: connections=" << connections
              << " workers=" << worker_count
              << " total_mib=" << mib
              << " seconds=" << seconds
              << " aggregate_mib_per_second=" << (mib / seconds)
              << " mib_per_second_per_connection=" << (mib / seconds / connections) << '\n';

    cleanup.store(true, std::memory_order_release);
    for (auto& client : clients) client.join();
    failures.check();
}

std::pair<size_t, size_t> process_memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)),
        "GetProcessMemoryInfo failed");
    return {counters.WorkingSetSize, counters.PrivateUsage};
}

DWORD process_handle_count() {
    DWORD count = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &count),
            "GetProcessHandleCount failed");
    return count;
}

void run_mixed_soak(size_t seconds, size_t worker_count) {
    const DWORD handles_before = process_handle_count();
    double elapsed_result = 0;
    double mib_result = 0;
    uint64_t churn_result = 0;
    {
        RelayFixture fixture(worker_count, RelayFixture::Mode::echo);
        FailureLog failures;
        std::atomic<bool> stop{false};
        std::atomic<size_t> ready{0};
        std::atomic<size_t> next{0};
        std::atomic<uint64_t> churn{0};
        std::atomic<uint64_t> transferred{0};
        constexpr size_t long_connections = 4;
        constexpr size_t churn_threads = 16;
        std::vector<std::thread> threads;

        for (size_t index = 0; index < long_connections; ++index) {
            threads.emplace_back([&, index] {
                bool announced = false;
                try {
                    auto client =
                        fixture.connect_client(static_cast<int64_t>(index + 1));
                    const SOCKET native = client.socket.native_handle();
                    set_native_timeouts(native);
                    ready.fetch_add(1, std::memory_order_release);
                    announced = true;
                    std::vector<char> send_buffer(32 * 1024, 'x');
                    std::atomic<bool> io_failed{false};
                    std::thread reader([&] {
                        std::vector<char> receive_buffer(32 * 1024);
                        while (!stop.load(std::memory_order_acquire) &&
                               !io_failed.load(std::memory_order_acquire)) {
                            const int result = ::recv(
                                native, receive_buffer.data(),
                                static_cast<int>(receive_buffer.size()), 0);
                            if (result <= 0) {
                                if (!stop.load(std::memory_order_acquire)) {
                                    failures.add("mixed soak reader stopped "
                                                 "unexpectedly");
                                    io_failed.store(true,
                                                    std::memory_order_release);
                                    ::shutdown(native, SD_BOTH);
                                }
                                break;
                            }
                            transferred.fetch_add(static_cast<size_t>(result),
                                                  std::memory_order_relaxed);
                        }
                    });
                    while (!stop.load(std::memory_order_acquire) &&
                           !io_failed.load(std::memory_order_acquire)) {
                        const int result =
                            ::send(native, send_buffer.data(),
                                   static_cast<int>(send_buffer.size()), 0);
                        if (result <= 0) {
                            if (!stop.load(std::memory_order_acquire)) {
                                failures.add(
                                    "mixed soak writer stopped unexpectedly");
                                io_failed.store(true,
                                                std::memory_order_release);
                                ::shutdown(native, SD_BOTH);
                            }
                            break;
                        }
                        transferred.fetch_add(static_cast<size_t>(result),
                                              std::memory_order_relaxed);
                    }
                    reader.join();
                    fixture.close_client(client, false);
                    fixture.require_cleanup(client.port);
                } catch (const std::exception &e) {
                    failures.add(e.what());
                    if (!announced)
                        ready.fetch_add(1, std::memory_order_release);
                }
            });
        }

        const auto ready_deadline = Clock::now() + 30s;
        while (ready.load(std::memory_order_acquire) != long_connections) {
            require(Clock::now() < ready_deadline,
                    "mixed soak clients failed to connect");
            std::this_thread::sleep_for(1ms);
        }

        // Exercise relay lifetime without turning the soak into a test of
        // Windows loopback port reuse under unbounded short-connection churn.
        const auto churn_started = Clock::now();
        constexpr auto churn_interval = 10ms;
        for (size_t index = 0; index < churn_threads; ++index) {
            threads.emplace_back([&] {
                try {
                    while (!stop.load(std::memory_order_acquire) &&
                           !failures.failed.load(std::memory_order_acquire)) {
                        const size_t connection =
                            next.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::sleep_until(
                            churn_started + connection * churn_interval);
                        if (stop.load(std::memory_order_acquire) ||
                            failures.failed.load(std::memory_order_acquire)) {
                            break;
                        }
                        auto client = fixture.connect_client(
                            static_cast<int64_t>(100 + connection));
                        try {
                            fixture.exchange_byte(client);
                        } catch (const std::exception &e) {
                            throw std::runtime_error(
                                "mixed soak churn exchange failed for source port " +
                                std::to_string(client.port) + ": " + e.what());
                        }
                        fixture.close_client(client, true);
                        fixture.require_cleanup(client.port);
                        churn.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (const std::exception &e) {
                    failures.add(e.what());
                }
            });
        }

        const auto started = Clock::now();
        while (Clock::now() - started < std::chrono::seconds(seconds) &&
               !failures.failed.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(50ms);
        }
        stop.store(true, std::memory_order_release);
        for (auto &thread : threads)
            thread.join();
        threads.clear();
        failures.check();
        elapsed_result =
            std::chrono::duration<double>(Clock::now() - started).count();
        mib_result =
            static_cast<double>(transferred.load()) / (1024.0 * 1024.0);
        churn_result = churn.load();
    }
    const DWORD handles_after = process_handle_count();
    std::this_thread::sleep_for(500ms);
    const DWORD handles_settled = process_handle_count();

    std::cout << std::unitbuf << std::fixed << std::setprecision(2)
              << "MEASURE mixed_soak: seconds=" << elapsed_result
              << " churn_connections=" << churn_result
              << " transferred_mib=" << mib_result
              << " aggregate_mib_per_second=" << (mib_result / elapsed_result)
              << " handles_before=" << handles_before
              << " handles_after=" << handles_after
              << " handles_settled=" << handles_settled
              << " sessions_alive=" << clew::g_relay_sessions_alive.load()
              << '\n';
    require(handles_settled <= handles_before + 4,
            "handles did not return to baseline after mixed soak");
    std::cout << "PASS mixed_soak: handles returned to baseline\n";
}

void run_active_shutdown(size_t connections, size_t worker_count) {
    const DWORD handles_before = process_handle_count();
    DWORD handles_active = handles_before;
    {
        RelayFixture fixture(worker_count, RelayFixture::Mode::echo);
        std::vector<ConnectedClient> clients;
        clients.reserve(connections);
        for (size_t i = 0; i < connections; ++i) {
            clients.push_back(fixture.connect_client(static_cast<int64_t>(i + 1)));
        }
        fixture.require_ready(connections, connections);
        std::this_thread::sleep_for(200ms);
        handles_active = process_handle_count();
        require(handles_active >= handles_before + connections,
                "active shutdown test did not create enough socket handles");

        fixture.stop();
        clients.clear();
    }
    const DWORD handles_after = process_handle_count();
    std::cout << std::unitbuf << std::fixed << std::setprecision(2)
              << "MEASURE active_shutdown: connections=" << connections
              << " handles_before=" << handles_before
              << " handles_active=" << handles_active
              << " handles_after=" << handles_after << '\n';
    require(handles_after <= handles_before + 16,
            "socket handles leaked when active relay coroutines were destroyed");
    std::cout << "PASS active_shutdown: handles returned to baseline\n";
}

void run_idle_memory(size_t connections, size_t worker_count) {
    RelayFixture fixture(worker_count, RelayFixture::Mode::echo);

    for (size_t i = 0; i < 16; ++i) {
        auto client = fixture.connect_client(static_cast<int64_t>(i + 1));
        fixture.exchange_byte(client);
        fixture.close_client(client, false);
        fixture.require_cleanup(client.port);
    }
    fixture.mark_ready_baseline();
    const auto [working_set_before, private_before] = process_memory();

    std::vector<ConnectedClient> clients;
    clients.reserve(connections);
    for (size_t i = 0; i < connections; ++i) {
        clients.push_back(fixture.connect_client(static_cast<int64_t>(100 + i)));
    }
    fixture.require_ready(connections, connections);
    std::this_thread::sleep_for(200ms);
    const auto [working_set_after, private_after] = process_memory();

    const double working_set_delta = static_cast<double>(working_set_after) -
                                     static_cast<double>(working_set_before);
    const double private_delta = static_cast<double>(private_after) -
                                 static_cast<double>(private_before);
    std::cout << std::fixed << std::setprecision(2)
              << "PASS idle_memory: connections=" << connections
              << " working_set_delta_mib=" << (working_set_delta / (1024.0 * 1024.0))
              << " private_delta_mib=" << (private_delta / (1024.0 * 1024.0))
              << " private_delta_bytes_per_connection=" << (private_delta / connections) << '\n';

    for (auto& client : clients) {
        fixture.close_client(client, false);
        fixture.require_cleanup(client.port);
    }
}

struct Options {
    bool benchmark = false;
    bool shutdown_only = false;
    size_t iterations = 2000;
    size_t concurrency = 32;
    size_t workers = 4;
    size_t idle_connections = 1000;
    size_t throughput_mib = 32;
    size_t soak_seconds = 0;
};

size_t parse_size(const std::string& name, const std::string& value, size_t minimum) {
    const auto parsed = std::stoull(value);
    require(parsed >= minimum && parsed <= SIZE_MAX, "invalid " + name + ": " + value);
    return static_cast<size_t>(parsed);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--benchmark" || argument == "--shutdown-only") {
            if (argument == "--benchmark") options.benchmark = true;
            else options.shutdown_only = true;
            continue;
        }
        require(i + 1 < argc, "missing value for " + argument);
        const std::string value = argv[++i];
        if (argument == "--iterations") options.iterations = parse_size(argument, value, 1);
        else if (argument == "--concurrency") options.concurrency = parse_size(argument, value, 1);
        else if (argument == "--workers") options.workers = parse_size(argument, value, 2);
        else if (argument == "--idle") options.idle_connections = parse_size(argument, value, 1);
        else if (argument == "--throughput-mib") options.throughput_mib = parse_size(argument, value, 1);
        else if (argument == "--soak-seconds") options.soak_seconds = parse_size(argument, value, 1);
        else throw std::runtime_error("unknown argument: " + argument);
    }
    require(options.concurrency <= options.iterations,
            "concurrency cannot exceed iterations");
    return options;
}

} // namespace

int main(int argc, char** argv) {
    quill::Backend::start();
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("relay_test_console");
    clew::g_logger = quill::Frontend::create_or_get_logger("relay_test", std::move(sink));
    clew::g_logger->set_log_level(quill::LogLevel::Error);

    try {
        const Options options = parse_options(argc, argv);
        run_active_shutdown(64, options.workers);
        if (options.shutdown_only) return 0;
        if (options.soak_seconds > 0) {
            run_mixed_soak(options.soak_seconds, options.workers);
            return 0;
        }
        run_teardown_matrix(250, options.workers);
        if (options.benchmark) {
            const size_t bytes = options.throughput_mib * 1024 * 1024;
            run_throughput(1, bytes, options.workers);
            run_throughput(8, bytes, options.workers);
            run_idle_memory(options.idle_connections, options.workers);
        }
        run_abrupt_close_stress(options.iterations, options.concurrency, options.workers);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL test_relay: " << e.what() << '\n';
        return 1;
    }
}
