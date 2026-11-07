#include "static_files.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

StaticFiles::StaticFiles(const fs::path& root) {
    std::error_code ec;
    root_ = fs::canonical(root, ec);
    if (ec || !fs::is_directory(root_))
        throw std::runtime_error("document root is not a directory: " + root.string());
}

std::optional<fs::path> StaticFiles::resolve(const std::string& decoded_path) const {
    if (decoded_path.empty() || decoded_path[0] != '/') return std::nullopt;
    if (decoded_path.find('\0') != std::string::npos) return std::nullopt;
    if (decoded_path.find('\\') != std::string::npos) return std::nullopt;

    fs::path relative;
    std::size_t start = 1;
    while (start <= decoded_path.size()) {
        std::size_t slash = decoded_path.find('/', start);
        if (slash == std::string::npos) slash = decoded_path.size();
        const std::string segment = decoded_path.substr(start, slash - start);
        start = slash + 1;
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") return std::nullopt;
        relative /= segment;
    }

    fs::path candidate = root_ / relative;
    std::error_code ec;
    if (fs::is_directory(candidate, ec)) candidate /= "index.html";

    const fs::path real = fs::canonical(candidate, ec);
    if (ec) return std::nullopt;  // does not exist

    // The real path must still be inside the root (blocks symlink escapes).
    const std::string& r = root_.native();
    const std::string& p = real.native();
    if (p.compare(0, r.size(), r) != 0 ||
        (p.size() > r.size() && r.back() != '/' && p[r.size()] != '/'))
        return std::nullopt;

    if (!fs::is_regular_file(real, ec)) return std::nullopt;
    return real;
}

std::string StaticFiles::mime_type(const fs::path& file) {
    static const std::unordered_map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},  {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},    {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},          {".txt", "text/plain; charset=utf-8"},
        {".xml", "application/xml"},            {".svg", "image/svg+xml"},
        {".png", "image/png"},                  {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},                {".gif", "image/gif"},
        {".ico", "image/x-icon"},               {".webp", "image/webp"},
        {".pdf", "application/pdf"},            {".wasm", "application/wasm"},
    };
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

http::Response StaticFiles::serve(const http::Request& req) const {
    if (req.method != "GET" && req.method != "HEAD") {
        auto r = http::Response::error(405);
        r.headers["Allow"] = "GET, HEAD";
        return r;
    }

    const auto file = resolve(req.path);
    if (!file) return http::Response::error(404);

    std::ifstream in(*file, std::ios::binary);
    if (!in) return http::Response::error(500);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return http::Response::error(500);

    http::Response r;
    r.status = 200;
    r.body = std::move(data);
    r.headers["Content-Type"] = mime_type(*file);
    r.head_only = req.method == "HEAD";
    return r;
}
