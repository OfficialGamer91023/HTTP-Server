#include "server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "api.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};
std::mutex g_log_mutex;

void log_request(const std::string& method, const std::string& target, int status, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << method << ' ' << target << " -> " << status << " (" << bytes << " bytes)\n";
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void finalize(http::Response& resp, bool keep_alive) {
    resp.headers["Connection"] = keep_alive ? "keep-alive" : "close";
    resp.headers["Date"] = http::http_date();
    resp.headers["Server"] = "http-server/2.0";
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

void request_stop() { g_stop_requested.store(true); }

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      static_files_(config_.root),
      jobs_(config_.job_workers),
      pool_(config_.threads, config_.max_pending_connections) {}

void Server::stop() { stopping_.store(true); }

http::Response Server::route(const http::Request& req) {
    if (req.path == "/api/health") return handle_health(req, jobs_);
    if (req.path == "/api/jobs" || starts_with(req.path, "/api/jobs/")) return handle_jobs_api(req, jobs_);
    if (req.path == "/soap") return handle_soap(req, jobs_);
    return static_files_.serve(req);
}

void Server::handle_connection(int fd) {
    timeval tv{};
    tv.tv_sec = config_.keep_alive_timeout_s;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string buffer;
    char chunk[8192];
    int served = 0;

    while (!stopping_.load() && !g_stop_requested.load()) {
        http::Request req;
        const http::ParseResult parsed = http::parse_request(buffer, req, config_.limits);

        if (parsed.status == http::ParseStatus::Incomplete) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n > 0) {
                buffer.append(chunk, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            // Timed out mid-request: tell the client before hanging up.
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !buffer.empty()) {
                auto r = http::Response::error(408);
                finalize(r, false);
                send_all(fd, r.serialize());
            }
            return;  // client closed, idle timeout, or socket error
        }

        if (parsed.status == http::ParseStatus::Error) {
            auto r = http::Response::error(parsed.error_code);
            finalize(r, false);
            log_request("-", "(malformed request)", r.status, r.body.size());
            send_all(fd, r.serialize());
            return;
        }

        buffer.erase(0, parsed.consumed);  // keep any pipelined bytes
        ++served;

        http::Response resp;
        try {
            resp = route(req);
        } catch (const std::exception& e) {
            std::cerr << "handler error: " << e.what() << '\n';
            resp = http::Response::error(500);
        }
        if (req.method == "HEAD") resp.head_only = true;

        const bool keep = req.keep_alive() && served < config_.max_requests_per_connection &&
                          !stopping_.load() && !g_stop_requested.load();
        finalize(resp, keep);
        log_request(req.method, req.target, resp.status, resp.body.size());
        if (!send_all(fd, resp.serialize()) || !keep) return;
    }
}

int Server::run() {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        return 1;
    }
    const int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid bind address: " << config_.bind_address << '\n';
        ::close(listen_fd);
        return 1;
    }
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(listen_fd);
        return 1;
    }
    if (::listen(listen_fd, 128) < 0) {
        std::perror("listen");
        ::close(listen_fd);
        return 1;
    }

    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::cout << "Listening on http://" << config_.bind_address << ':' << ntohs(addr.sin_port)
              << " (root " << static_files_.root().string() << ", " << config_.threads
              << " threads)" << std::endl;

    while (!stopping_.load() && !g_stop_requested.load()) {
        pollfd pfd{listen_fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 250);  // wake up regularly to check the stop flag
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }
        if (ready == 0) continue;

        const int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0) continue;  // EINTR, ECONNABORTED, EMFILE...: keep serving

        const bool queued = pool_.try_submit([this, client] {
            handle_connection(client);
            ::close(client);
        });
        if (!queued) {  // overloaded: shed load instead of queueing forever
            auto r = http::Response::error(503);
            r.headers["Retry-After"] = "1";
            finalize(r, false);
            send_all(client, r.serialize());
            ::close(client);
        }
    }

    std::cout << "Shutting down..." << std::endl;
    ::close(listen_fd);
    pool_.shutdown();
    jobs_.shutdown();
    return 0;
}
