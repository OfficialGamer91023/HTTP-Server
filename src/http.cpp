#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string_view>

namespace http {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

// True if the comma-separated header value contains `token` (case-insensitive).
bool has_token(const std::string& value, const std::string& token) {
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        if (iequals(trim(std::string_view(value).substr(start, comma - start)), token)) return true;
        start = comma + 1;
    }
    return false;
}

// RFC 9110 "tchar": characters allowed in methods and header names.
bool is_tchar(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool is_token(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_tchar);
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](char c) { return c >= '0' && c <= '9'; });
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ParseResult fail(int code) { return {ParseStatus::Error, code, 0}; }

}  // namespace

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return lower(x) == lower(y); });
}

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
}

std::string Request::header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
}

bool Request::keep_alive() const {
    const std::string conn = header("Connection");
    if (version == "HTTP/1.0") return has_token(conn, "keep-alive");
    return !has_token(conn, "close");
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string http_date() {
    std::time_t now = std::time(nullptr);
    std::tm gmt{};
    gmtime_r(&now, &gmt);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return buf;
}

std::string Response::serialize() const {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
    for (const auto& [name, value] : headers) {
        if (iequals(name, "Content-Length")) continue;  // always computed below
        out += name + ": " + value + "\r\n";
    }
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    if (!head_only) out += body;
    return out;
}

Response Response::text(int status, std::string body, std::string content_type) {
    Response r;
    r.status = status;
    r.body = std::move(body);
    r.headers["Content-Type"] = std::move(content_type);
    return r;
}

Response Response::error(int status) {
    return text(status, std::to_string(status) + " " + reason_phrase(status) + "\n");
}

std::string url_decode(const std::string& s, bool& ok, bool plus_as_space) {
    ok = true;
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%') {
            if (i + 2 >= s.size()) {  // need two hex digits after '%'
                ok = false;
                return {};
            }
            int hi = hex_value(s[i + 1]), lo = hex_value(s[i + 2]);
            if (hi < 0 || lo < 0) {
                ok = false;
                return {};
            }
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else if (c == '+' && plus_as_space) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string query_param(const std::string& query, const std::string& key) {
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(start, amp - start);
        std::size_t eq = pair.find('=');
        bool ok1 = true, ok2 = true;
        std::string k = url_decode(pair.substr(0, eq), ok1, true);
        if (ok1 && k == key) {
            if (eq == std::string::npos) return "";
            std::string v = url_decode(pair.substr(eq + 1), ok2, true);
            return ok2 ? v : "";
        }
        start = amp + 1;
    }
    return "";
}

ParseResult parse_request(const std::string& buf, Request& out, const Limits& limits) {
    const std::size_t header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (buf.size() > limits.max_header_bytes) return fail(431);
        return {ParseStatus::Incomplete, 0, 0};
    }
    if (header_end + 4 > limits.max_header_bytes) return fail(431);

    Request req;
    const std::string_view head(buf.data(), header_end);

    // Request line: METHOD SP TARGET SP VERSION
    const std::size_t line_end = std::min(head.find("\r\n"), head.size());
    const std::string_view request_line = head.substr(0, line_end);
    const std::size_t sp1 = request_line.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || request_line.find(' ', sp2 + 1) != std::string_view::npos)
        return fail(400);

    req.method = std::string(request_line.substr(0, sp1));
    req.target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
    req.version = std::string(request_line.substr(sp2 + 1));

    if (!is_token(req.method)) return fail(400);
    if (req.version.rfind("HTTP/", 0) != 0) return fail(400);
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") return fail(505);
    if (req.target.empty() || req.target[0] != '/') return fail(400);

    const std::size_t qmark = req.target.find('?');
    bool ok = true;
    req.path = url_decode(req.target.substr(0, qmark), ok);
    if (!ok || req.path.find('\0') != std::string::npos) return fail(400);
    if (qmark != std::string::npos) req.query = req.target.substr(qmark + 1);

    // Header fields
    std::size_t pos = line_end + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view line = head.substr(pos, next - pos);
        pos = next + 2;

        if (line.empty()) continue;
        if (line[0] == ' ' || line[0] == '\t') return fail(400);  // obsolete line folding
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) return fail(400);
        const std::string name(line.substr(0, colon));
        if (!is_token(name)) return fail(400);  // also rejects "Name :" (space before colon)
        const std::string value = trim(line.substr(colon + 1));

        auto it = req.headers.find(name);
        if (it == req.headers.end()) {
            req.headers.emplace(name, value);
        } else if (iequals(name, "Content-Length") || iequals(name, "Host")) {
            return fail(400);  // duplicates here enable request smuggling
        } else {
            it->second += ", " + value;
        }
    }

    if (req.version == "HTTP/1.1" && req.headers.find("Host") == req.headers.end()) return fail(400);
    // Chunked uploads are not supported; refusing is safer than guessing the body length.
    if (req.headers.find("Transfer-Encoding") != req.headers.end()) return fail(501);

    std::size_t content_length = 0;
    const std::string cl = req.header("Content-Length");
    if (!cl.empty()) {
        if (!all_digits(cl) || cl.size() > 18) return fail(400);
        content_length = std::stoull(cl);
        if (content_length > limits.max_body_bytes) return fail(413);
    }

    const std::size_t body_start = header_end + 4;
    if (buf.size() - body_start < content_length) return {ParseStatus::Incomplete, 0, 0};

    req.body = buf.substr(body_start, content_length);
    out = std::move(req);
    return {ParseStatus::Complete, 0, body_start + content_length};
}

}  // namespace http
