// HTTP/1.1 request parsing and response serialization.
#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace http {

// Header names are case-insensitive (RFC 9110 section 5.1).
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};
using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;

struct Request {
    std::string method;   // "GET", "POST", ...
    std::string target;   // raw request target, e.g. "/a%20b?x=1"
    std::string path;     // percent-decoded path, e.g. "/a b"
    std::string query;    // raw query string without '?', e.g. "x=1"
    std::string version;  // "HTTP/1.1" or "HTTP/1.0"
    Headers headers;
    std::string body;

    // Returns the header value, or "" if absent.
    std::string header(const std::string& name) const;
    // HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close.
    bool keep_alive() const;
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;
    bool head_only = false;  // HEAD: send headers (incl. Content-Length) but no body

    std::string serialize() const;

    static Response text(int status, std::string body,
                         std::string content_type = "text/plain; charset=utf-8");
    static Response error(int status);
};

const char* reason_phrase(int status);
std::string http_date();  // current time in IMF-fixdate format

enum class ParseStatus { Incomplete, Complete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    int error_code = 0;    // HTTP status to send back when status == Error
    std::size_t consumed = 0;  // bytes of the buffer used by this request
};

struct Limits {
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_body_bytes = 1024 * 1024;
};

// Tries to parse one request from the front of `buf`. The buffer may hold a
// partial request (returns Incomplete) or several pipelined requests (only the
// first is parsed; `consumed` says where the next one starts).
ParseResult parse_request(const std::string& buf, Request& out, const Limits& limits = {});

// Percent-decodes `s`. Sets ok=false on a malformed escape. If plus_as_space,
// '+' decodes to ' ' (form/query encoding).
std::string url_decode(const std::string& s, bool& ok, bool plus_as_space = false);

// Returns the decoded value of `key` in a query string, or "" if missing.
std::string query_param(const std::string& query, const std::string& key);

bool iequals(const std::string& a, const std::string& b);

}  // namespace http
