#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "server.hpp"

namespace {

void on_signal(int) { request_stop(); }

void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --port N        port to listen on (default 8080, 0 = any free port)\n"
              << "  --bind ADDR     IPv4 address to bind (default 127.0.0.1)\n"
              << "  --root DIR      directory to serve files from (default ./public)\n"
              << "  --threads N     connection worker threads (default 8)\n"
              << "  --job-workers N background job workers (default 2)\n"
              << "  --help          show this message\n";
}

bool parse_number(const char* s, long min, long max, long& out) {
    char* end = nullptr;
    errno = 0;
    out = std::strtol(s, &end, 10);
    return errno == 0 && end != s && *end == '\0' && out >= min && out <= max;
}

}  // namespace

int main(int argc, char** argv) {
    ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
        const char* value = argv[++i];
        long n = 0;
        if (arg == "--port" && parse_number(value, 0, 65535, n)) {
            config.port = static_cast<std::uint16_t>(n);
        } else if (arg == "--threads" && parse_number(value, 1, 1024, n)) {
            config.threads = static_cast<std::size_t>(n);
        } else if (arg == "--job-workers" && parse_number(value, 1, 64, n)) {
            config.job_workers = static_cast<std::size_t>(n);
        } else if (arg == "--bind") {
            config.bind_address = value;
        } else if (arg == "--root") {
            config.root = value;
        } else {
            std::cerr << "invalid option or value: " << arg << ' ' << value << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // a client hanging up must not kill the server

    try {
        Server server(config);
        return server.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
