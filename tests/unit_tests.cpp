// Dependency-free unit tests. Run with `make unit`.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "api.hpp"
#include "http.hpp"
#include "jobs.hpp"
#include "static_files.hpp"
#include "xml.hpp"

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// ---- HTTP parsing ----------------------------------------------------------

static void test_parse_simple_get() {
    http::Request r;
    auto res = http::parse_request("GET /a%20b?x=1 HTTP/1.1\r\nHost: h\r\nX-Test:  v  \r\n\r\n", r);
    CHECK(res.status == http::ParseStatus::Complete);
    CHECK_EQ(r.method, "GET");
    CHECK_EQ(r.path, "/a b");
    CHECK_EQ(r.query, "x=1");
    CHECK_EQ(r.header("x-test"), "v");  // case-insensitive, trimmed
    CHECK(r.keep_alive());
}

static void test_parse_incomplete_and_body() {
    http::Request r;
    std::string req = "POST /api/jobs HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhel";
    CHECK(http::parse_request(req, r).status == http::ParseStatus::Incomplete);
    CHECK(http::parse_request("GET / HTTP/1.1\r\nHost:", r).status == http::ParseStatus::Incomplete);
    req += "lo";
    auto res = http::parse_request(req, r);
    CHECK(res.status == http::ParseStatus::Complete);
    CHECK_EQ(r.body, "hello");
    CHECK_EQ(res.consumed, req.size());
}

static void test_parse_pipelined() {
    http::Request r;
    const std::string first = "GET /1 HTTP/1.1\r\nHost: h\r\n\r\n";
    const std::string both = first + "GET /2 HTTP/1.1\r\nHost: h\r\n\r\n";
    auto res = http::parse_request(both, r);
    CHECK(res.status == http::ParseStatus::Complete);
    CHECK_EQ(r.path, "/1");
    CHECK_EQ(res.consumed, first.size());
    http::parse_request(both.substr(res.consumed), r);
    CHECK_EQ(r.path, "/2");
}

static int error_code(const std::string& raw, http::Limits limits = {}) {
    http::Request r;
    auto res = http::parse_request(raw, r, limits);
    return res.status == http::ParseStatus::Error ? res.error_code : 0;
}

static void test_parse_errors() {
    CHECK_EQ(error_code("GET / HTTP/1.1\r\n\r\n"), 400);                         // no Host
    CHECK_EQ(error_code("GET /\r\nHost: h\r\n\r\n"), 400);                        // no version
    CHECK_EQ(error_code("GET  / HTTP/1.1\r\nHost: h\r\n\r\n"), 400);              // double space
    CHECK_EQ(error_code("GET / HTTP/2.0\r\nHost: h\r\n\r\n"), 505);
    CHECK_EQ(error_code("GET example.com HTTP/1.1\r\nHost: h\r\n\r\n"), 400);     // not origin-form
    CHECK_EQ(error_code("GET /%zz HTTP/1.1\r\nHost: h\r\n\r\n"), 400);            // bad escape
    CHECK_EQ(error_code("GET /%00 HTTP/1.1\r\nHost: h\r\n\r\n"), 400);            // NUL byte
    CHECK_EQ(error_code("GET / HTTP/1.1\r\nHost: h\r\nBad Header: x\r\n\r\n"), 400);
    CHECK_EQ(error_code("GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n"), 400);
    CHECK_EQ(error_code("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n"), 400);
    CHECK_EQ(error_code("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"), 400);
    CHECK_EQ(error_code("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);

    http::Limits small;
    small.max_body_bytes = 10;
    small.max_header_bytes = 64;
    CHECK_EQ(error_code("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\n", small), 413);
    CHECK_EQ(error_code("GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(100, 'a'), small), 431);
}

static void test_keep_alive_rules() {
    http::Request r;
    http::parse_request("GET / HTTP/1.0\r\n\r\n", r);
    CHECK(!r.keep_alive());
    http::parse_request("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", r);
    CHECK(r.keep_alive());
    http::parse_request("GET / HTTP/1.1\r\nHost: h\r\nConnection: upgrade, close\r\n\r\n", r);
    CHECK(!r.keep_alive());
}

static void test_response_serialize() {
    auto r = http::Response::text(404, "nope");
    const std::string s = r.serialize();
    CHECK(s.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    CHECK(contains(s, "Content-Length: 4\r\n"));
    CHECK(s.size() >= 4 && s.compare(s.size() - 4, 4, "nope") == 0);
    r.head_only = true;
    const std::string h = r.serialize();
    CHECK(contains(h, "Content-Length: 4\r\n"));  // HEAD keeps the real length
    CHECK(h.compare(h.size() - 4, 4, "\r\n\r\n") == 0);
}

static void test_query_and_decode() {
    CHECK_EQ(http::query_param("a=1&delay_ms=250&b", "delay_ms"), "250");
    CHECK_EQ(http::query_param("q=hello+world%21", "q"), "hello world!");
    CHECK_EQ(http::query_param("a=1", "missing"), "");
    bool ok = true;
    http::url_decode("abc%4", ok);
    CHECK(!ok);
}

// ---- Static files ----------------------------------------------------------

static void test_static_resolve() {
    const fs::path base = fs::temp_directory_path() / "http_server_unit";
    fs::remove_all(base);
    fs::create_directories(base / "root" / "sub");
    std::ofstream(base / "root" / "index.html") << "<h1>hi</h1>";
    std::ofstream(base / "root" / "sub" / "a.css") << "body{}";
    std::ofstream(base / "secret.txt") << "secret";
    fs::create_symlink(base / "secret.txt", base / "root" / "link.txt");

    StaticFiles files(base / "root");
    CHECK(files.resolve("/").has_value());               // directory -> index.html
    CHECK(files.resolve("/sub/a.css").has_value());
    CHECK(files.resolve("//sub/./a.css").has_value());
    CHECK(!files.resolve("/../secret.txt").has_value());  // traversal
    CHECK(!files.resolve("/sub/../../secret.txt").has_value());
    CHECK(!files.resolve("/link.txt").has_value());       // symlink escaping the root
    CHECK(!files.resolve("/missing.html").has_value());
    CHECK(!files.resolve("/sub").has_value());            // directory without index.html

    CHECK_EQ(StaticFiles::mime_type("x.CSS"), "text/css; charset=utf-8");
    CHECK_EQ(StaticFiles::mime_type("x.png"), "image/png");
    CHECK_EQ(StaticFiles::mime_type("x.unknown"), "application/octet-stream");

    http::Request req;
    req.method = "DELETE";
    req.path = "/";
    CHECK_EQ(files.serve(req).status, 405);
    req.method = "GET";
    auto resp = files.serve(req);
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(resp.body, "<h1>hi</h1>");
    fs::remove_all(base);
}

// ---- XML -------------------------------------------------------------------

static void test_xml_parse() {
    std::string err;
    auto root = xml::parse(
        "<?xml version=\"1.0\"?><!-- c --><a:root xmlns:a=\"urn:a\" k='v &amp; w'>"
        "<a:child>1 &lt; 2 &#x263A;</a:child><b><![CDATA[<raw>]]></b><empty/></a:root>",
        err);
    CHECK(root != nullptr);
    if (!root) return;
    CHECK_EQ(root->local_name(), "root");
    CHECK_EQ(root->namespace_uri(), "urn:a");
    CHECK_EQ(root->attributes.at("k"), "v & w");
    CHECK_EQ(root->child_text("child"), "1 < 2 \xE2\x98\xBA");
    CHECK_EQ(root->child("child")->namespace_uri(), "urn:a");  // inherited declaration
    CHECK_EQ(root->child_text("b"), "<raw>");
    CHECK(root->child("empty") != nullptr);
}

static void test_xml_rejects_bad_input() {
    const char* bad[] = {
        "",
        "<a>",
        "<a></b>",
        "<a x=1/>",
        "<a x='1' x='2'/>",
        "<a>&unknown;</a>",
        "<a>&#0;</a>",
        "<a/><b/>",
        "<!DOCTYPE a [<!ENTITY x \"y\">]><a>&x;</a>",
        "<a><!DOCTYPE b></a>",
    };
    for (const char* input : bad) {
        std::string err;
        CHECK(xml::parse(input, err) == nullptr);
        CHECK(!err.empty());
    }
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "<a>";
    std::string err;
    CHECK(xml::parse(deep, err) == nullptr);  // depth limit
    CHECK_EQ(xml::escape("<a href=\"x\">&'"), "&lt;a href=&quot;x&quot;&gt;&amp;&apos;");
}

// ---- Jobs ------------------------------------------------------------------

static bool wait_done(JobQueue& q, std::uint64_t id, Job& out) {
    for (int i = 0; i < 200; ++i) {
        auto j = q.get(id);
        if (j && (j->state == JobState::Done || j->state == JobState::Failed)) {
            out = *j;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static void test_count_text() {
    std::size_t l, w, b;
    count_text("hello world\nsecond line\n", l, w, b);
    CHECK_EQ(l, 2u);
    CHECK_EQ(w, 4u);
    CHECK_EQ(b, 24u);
    count_text("", l, w, b);
    CHECK(l == 0 && w == 0 && b == 0);
}

static void test_job_queue() {
    JobQueue q(2, 3);
    auto id = q.submit("one two three\n");
    CHECK(id.has_value());
    Job job;
    CHECK(wait_done(q, *id, job));
    CHECK(job.state == JobState::Done);
    CHECK_EQ(job.words, 3u);
    CHECK(!q.get(999999).has_value());

    // Two slow jobs occupy both workers, so the pending limit (3) is reachable.
    q.submit("", 300);
    q.submit("", 300);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int accepted = 0;
    for (int i = 0; i < 5; ++i)
        if (q.submit("x")) ++accepted;
    CHECK_EQ(accepted, 3);
}

static void test_eviction() {
    JobQueue q(1, 100, 2);
    auto a = q.submit("a");
    Job j;
    wait_done(q, *a, j);
    auto b = q.submit("b");
    wait_done(q, *b, j);
    auto c = q.submit("c");
    wait_done(q, *c, j);
    CHECK(!q.get(*a).has_value());  // oldest finished job evicted
    CHECK(q.get(*c).has_value());
}

// ---- REST and SOAP handlers -------------------------------------------------

static http::Request make(const std::string& method, const std::string& path, const std::string& body = "",
                          const std::string& query = "") {
    http::Request r;
    r.method = method;
    r.path = path;
    r.target = path;
    r.query = query;
    r.version = "HTTP/1.1";
    r.body = body;
    r.headers["Host"] = "localhost:8080";
    r.headers["Content-Type"] = "text/xml; charset=utf-8";
    return r;
}

static std::string soap(const std::string& op_xml) {
    return "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "xmlns:j=\"urn:httpserver:jobs\"><soap:Body>" + op_xml + "</soap:Body></soap:Envelope>";
}

static void test_rest_api() {
    JobQueue q;
    auto created = handle_jobs_api(make("POST", "/api/jobs", "a b c"), q);
    CHECK_EQ(created.status, 202);
    CHECK_EQ(created.headers["Location"], "/api/jobs/1");
    Job j;
    CHECK(wait_done(q, 1, j));
    auto got = handle_jobs_api(make("GET", "/api/jobs/1"), q);
    CHECK_EQ(got.status, 200);
    CHECK(contains(got.body, "\"status\":\"done\""));
    CHECK(contains(got.body, "\"words\":3"));
    CHECK_EQ(handle_jobs_api(make("GET", "/api/jobs/42"), q).status, 404);
    CHECK_EQ(handle_jobs_api(make("GET", "/api/jobs/abc"), q).status, 404);
    CHECK_EQ(handle_jobs_api(make("GET", "/api/jobs"), q).status, 405);
    CHECK_EQ(handle_jobs_api(make("POST", "/api/jobs", "", "delay_ms=999999"), q).status, 400);
    CHECK_EQ(json_escape("a\"b\\\n\x01"), "a\\\"b\\\\\\n\\u0001");
}

static void test_soap_api() {
    JobQueue q;
    auto wsdl = handle_soap(make("GET", "/soap", "", "wsdl"), q);
    CHECK_EQ(wsdl.status, 200);
    CHECK(contains(wsdl.body, "location=\"http://localhost:8080/soap\""));
    std::string err;
    CHECK(xml::parse(wsdl.body, err) != nullptr);  // the WSDL is well-formed

    auto sub = handle_soap(make("POST", "/soap", soap("<j:SubmitJob><j:text>x &amp; y</j:text></j:SubmitJob>")), q);
    CHECK_EQ(sub.status, 200);
    CHECK(contains(sub.body, "<tns:jobId>1</tns:jobId>"));
    Job j;
    CHECK(wait_done(q, 1, j));
    CHECK_EQ(j.bytes, 5u);  // entity decoded before counting: "x & y"

    auto st = handle_soap(make("POST", "/soap", soap("<j:GetJobStatus><j:jobId> 1 </j:jobId></j:GetJobStatus>")), q);
    CHECK_EQ(st.status, 200);
    CHECK(contains(st.body, "<tns:status>done</tns:status>"));
    CHECK(contains(st.body, "<tns:words>3</tns:words>"));

    auto fault_of = [&](const std::string& body) { return handle_soap(make("POST", "/soap", body), q); };
    auto f = fault_of("<not xml");
    CHECK_EQ(f.status, 500);
    CHECK(contains(f.body, "soap:Client"));
    CHECK(contains(fault_of("<Envelope xmlns=\"urn:wrong\"><Body/></Envelope>").body, "VersionMismatch"));
    CHECK(contains(fault_of(soap("<j:Nope/>")).body, "unknown operation"));
    CHECK(contains(fault_of(soap("<x:SubmitJob xmlns:x=\"urn:other\"/>")).body, "namespace"));
    CHECK(contains(fault_of(soap("<j:GetJobStatus><j:jobId>77</j:jobId></j:GetJobStatus>")).body, "no job"));
    CHECK(contains(fault_of(soap("<j:SubmitJob><j:text/><j:delayMs>-5</j:delayMs></j:SubmitJob>")).body, "delayMs"));

    auto wrong_type = make("POST", "/soap", soap("<j:GetJobStatus/>"));
    wrong_type.headers["Content-Type"] = "application/json";
    CHECK_EQ(handle_soap(wrong_type, q).status, 415);
}

int main() {
    test_parse_simple_get();
    test_parse_incomplete_and_body();
    test_parse_pipelined();
    test_parse_errors();
    test_keep_alive_rules();
    test_response_serialize();
    test_query_and_decode();
    test_static_resolve();
    test_xml_parse();
    test_xml_rejects_bad_input();
    test_count_text();
    test_job_queue();
    test_eviction();
    test_rest_api();
    test_soap_api();

    std::cout << (g_checks - g_failures) << '/' << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
