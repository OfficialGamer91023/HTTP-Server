// Accept loop, per-connection keep-alive handling and request routing.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "http.hpp"
#include "jobs.hpp"
#include "static_files.hpp"
#include "thread_pool.hpp"

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 8080;  // 0 = pick a free port
    std::filesystem::path root = "public";
    std::size_t threads = 8;
    std::size_t max_pending_connections = 256;
    std::size_t job_workers = 2;
    int keep_alive_timeout_s = 5;
    int max_requests_per_connection = 100;
    http::Limits limits;
};

class Server {
public:
    explicit Server(ServerConfig config);

    // Blocks until stop() is called (or a signal calls request_stop()).
    int run();
    void stop();

    http::Response route(const http::Request& req);

private:
    void handle_connection(int fd);

    ServerConfig config_;
    StaticFiles static_files_;
    JobQueue jobs_;    // declared before pool_ so connection tasks die first
    ThreadPool pool_;
    std::atomic<bool> stopping_{false};
};

// Async-signal-safe: sets a flag the accept loop polls.
void request_stop();
