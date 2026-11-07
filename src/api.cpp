#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>

#include "xml.hpp"

namespace {

constexpr const char* kSoapEnvelopeNs = "http://schemas.xmlsoap.org/soap/envelope/";
constexpr const char* kServiceNs = "urn:httpserver:jobs";

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Parses a non-negative decimal integer with no sign, spaces or overflow.
std::optional<std::uint64_t> parse_uint(const std::string& s, std::uint64_t max) {
    if (s.empty() || s.size() > 19) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > max) return std::nullopt;
    return v;
}

http::Response json(int status, const std::string& body) {
    return http::Response::text(status, body + "\n", "application/json");
}

http::Response json_error(int status, const std::string& message) {
    return json(status, "{\"error\":\"" + json_escape(message) + "\"}");
}

http::Response method_not_allowed(const std::string& allow) {
    auto r = json_error(405, "method not allowed");
    r.headers["Allow"] = allow;
    return r;
}

std::string job_json(const Job& job) {
    std::string out = "{\"id\":" + std::to_string(job.id) + ",\"status\":\"" + to_string(job.state) + "\"";
    if (job.state == JobState::Done) {
        out += ",\"result\":{\"lines\":" + std::to_string(job.lines) +
               ",\"words\":" + std::to_string(job.words) +
               ",\"bytes\":" + std::to_string(job.bytes) + "}";
    } else if (job.state == JobState::Failed) {
        out += ",\"error\":\"" + json_escape(job.error) + "\"";
    }
    return out + "}";
}

// ---- SOAP helpers ---------------------------------------------------------

std::string envelope(const std::string& body_xml) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") +
           "<soap:Envelope xmlns:soap=\"" + kSoapEnvelopeNs + "\" xmlns:tns=\"" + kServiceNs + "\">\n"
           "  <soap:Body>\n" + body_xml + "  </soap:Body>\n</soap:Envelope>\n";
}

http::Response soap_response(int status, const std::string& body_xml) {
    return http::Response::text(status, envelope(body_xml), "text/xml; charset=utf-8");
}

// SOAP 1.1 faults travel with HTTP 500 (SOAP 1.1 section 6.2).
http::Response soap_fault(const std::string& code, const std::string& message) {
    return soap_response(500, "    <soap:Fault>\n      <faultcode>soap:" + code +
                                  "</faultcode>\n      <faultstring>" + xml::escape(message) +
                                  "</faultstring>\n    </soap:Fault>\n");
}

std::string tag(const std::string& name, const std::string& value) {
    return "      <tns:" + name + ">" + xml::escape(value) + "</tns:" + name + ">\n";
}

// Host header is echoed into the WSDL, so only allow plain host[:port].
std::string safe_host(const std::string& host) {
    if (host.empty() || host.size() > 255) return "localhost";
    for (char c : host)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != ':' &&
            c != '[' && c != ']')
            return "localhost";
    return host;
}

http::Response soap_submit_job(const xml::Element& op, JobQueue& jobs) {
    const xml::Element* text = op.child("text");
    if (!text) return soap_fault("Client", "SubmitJob requires a <text> element");

    unsigned delay = 0;
    if (const xml::Element* d = op.child("delayMs")) {
        auto v = parse_uint(trim(d->text), JobQueue::kMaxDelayMs);
        if (!v) return soap_fault("Client", "delayMs must be an integer from 0 to " +
                                                std::to_string(JobQueue::kMaxDelayMs));
        delay = static_cast<unsigned>(*v);
    }

    auto id = jobs.submit(text->text, delay);
    if (!id) return soap_fault("Server", "job queue is full, retry later");
    return soap_response(200, "    <tns:SubmitJobResponse>\n" + tag("jobId", std::to_string(*id)) +
                                  tag("status", "queued") + "    </tns:SubmitJobResponse>\n");
}

http::Response soap_get_job_status(const xml::Element& op, const JobQueue& jobs) {
    auto id = parse_uint(trim(op.child_text("jobId")), UINT64_MAX);
    if (!id) return soap_fault("Client", "GetJobStatus requires a numeric <jobId>");
    auto job = jobs.get(*id);
    if (!job) return soap_fault("Client", "no job with id " + std::to_string(*id));

    std::string body = tag("jobId", std::to_string(job->id)) + tag("status", to_string(job->state));
    if (job->state == JobState::Done) {
        body += tag("lines", std::to_string(job->lines)) + tag("words", std::to_string(job->words)) +
                tag("bytes", std::to_string(job->bytes));
    } else if (job->state == JobState::Failed) {
        body += tag("error", job->error);
    }
    return soap_response(200, "    <tns:GetJobStatusResponse>\n" + body + "    </tns:GetJobStatusResponse>\n");
}

}  // namespace

std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

http::Response handle_health(const http::Request& req, const JobQueue& jobs) {
    if (req.method != "GET" && req.method != "HEAD") return method_not_allowed("GET, HEAD");
    auto r = json(200, "{\"status\":\"ok\",\"pending_jobs\":" + std::to_string(jobs.pending()) + "}");
    r.head_only = req.method == "HEAD";
    return r;
}

http::Response handle_jobs_api(const http::Request& req, JobQueue& jobs) {
    const std::string rest = req.path.substr(std::string("/api/jobs").size());

    if (rest.empty() || rest == "/") {
        if (req.method != "POST") return method_not_allowed("POST");
        unsigned delay = 0;
        const std::string d = http::query_param(req.query, "delay_ms");
        if (!d.empty()) {
            auto v = parse_uint(d, JobQueue::kMaxDelayMs);
            if (!v)
                return json_error(400, "delay_ms must be an integer from 0 to " +
                                           std::to_string(JobQueue::kMaxDelayMs));
            delay = static_cast<unsigned>(*v);
        }
        auto id = jobs.submit(req.body, delay);
        if (!id) {
            auto r = json_error(503, "job queue is full, retry later");
            r.headers["Retry-After"] = "1";
            return r;
        }
        const std::string url = "/api/jobs/" + std::to_string(*id);
        auto r = json(202, "{\"id\":" + std::to_string(*id) +
                               ",\"status\":\"queued\",\"status_url\":\"" + url + "\"}");
        r.headers["Location"] = url;
        return r;
    }

    auto id = parse_uint(rest.substr(1), UINT64_MAX);
    if (rest[0] != '/' || !id) return json_error(404, "not found");
    if (req.method != "GET") return method_not_allowed("GET");
    auto job = jobs.get(*id);
    if (!job) return json_error(404, "no job with id " + std::to_string(*id));
    return json(200, job_json(*job));
}

http::Response handle_soap(const http::Request& req, JobQueue& jobs) {
    if (req.method == "GET") {
        if (!http::iequals(req.query, "wsdl")) {
            auto r = http::Response::text(405, "POST SOAP requests here, or GET /soap?wsdl\n");
            r.headers["Allow"] = "POST";
            return r;
        }
        const std::string url = "http://" + safe_host(req.header("Host")) + "/soap";
        return http::Response::text(200, build_wsdl(url), "text/xml; charset=utf-8");
    }
    if (req.method != "POST") {
        auto r = http::Response::error(405);
        r.headers["Allow"] = "GET, POST";
        return r;
    }

    std::string ctype = req.header("Content-Type");
    std::transform(ctype.begin(), ctype.end(), ctype.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ctype.rfind("text/xml", 0) != 0)
        return http::Response::text(415, "SOAP 1.1 requests must use Content-Type: text/xml\n");

    std::string error;
    auto root = xml::parse(req.body, error);
    if (!root) return soap_fault("Client", "malformed XML: " + error);
    if (root->local_name() != "Envelope") return soap_fault("Client", "root element must be Envelope");
    if (root->namespace_uri() != kSoapEnvelopeNs)
        return soap_fault("VersionMismatch", "expected SOAP 1.1 envelope namespace");

    const xml::Element* body = root->child("Body");
    if (!body || body->namespace_uri() != kSoapEnvelopeNs)
        return soap_fault("Client", "missing soap:Body");
    const xml::Element* op = body->first_child_element();
    if (!op) return soap_fault("Client", "soap:Body is empty");
    if (op->namespace_uri() != kServiceNs)
        return soap_fault("Client", "unknown operation namespace '" + op->namespace_uri() + "'");

    if (op->local_name() == "SubmitJob") return soap_submit_job(*op, jobs);
    if (op->local_name() == "GetJobStatus") return soap_get_job_status(*op, jobs);
    return soap_fault("Client", "unknown operation '" + op->local_name() + "'");
}

std::string build_wsdl(const std::string& endpoint_url) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<definitions name="JobService"
    targetNamespace="urn:httpserver:jobs"
    xmlns="http://schemas.xmlsoap.org/wsdl/"
    xmlns:soap="http://schemas.xmlsoap.org/wsdl/soap/"
    xmlns:tns="urn:httpserver:jobs"
    xmlns:xsd="http://www.w3.org/2001/XMLSchema">

  <types>
    <xsd:schema targetNamespace="urn:httpserver:jobs" elementFormDefault="qualified">
      <xsd:element name="SubmitJob">
        <xsd:complexType>
          <xsd:sequence>
            <xsd:element name="text" type="xsd:string"/>
            <xsd:element name="delayMs" type="xsd:unsignedInt" minOccurs="0"/>
          </xsd:sequence>
        </xsd:complexType>
      </xsd:element>
      <xsd:element name="SubmitJobResponse">
        <xsd:complexType>
          <xsd:sequence>
            <xsd:element name="jobId" type="xsd:unsignedLong"/>
            <xsd:element name="status" type="xsd:string"/>
          </xsd:sequence>
        </xsd:complexType>
      </xsd:element>
      <xsd:element name="GetJobStatus">
        <xsd:complexType>
          <xsd:sequence>
            <xsd:element name="jobId" type="xsd:unsignedLong"/>
          </xsd:sequence>
        </xsd:complexType>
      </xsd:element>
      <xsd:element name="GetJobStatusResponse">
        <xsd:complexType>
          <xsd:sequence>
            <xsd:element name="jobId" type="xsd:unsignedLong"/>
            <xsd:element name="status" type="xsd:string"/>
            <xsd:element name="lines" type="xsd:unsignedLong" minOccurs="0"/>
            <xsd:element name="words" type="xsd:unsignedLong" minOccurs="0"/>
            <xsd:element name="bytes" type="xsd:unsignedLong" minOccurs="0"/>
            <xsd:element name="error" type="xsd:string" minOccurs="0"/>
          </xsd:sequence>
        </xsd:complexType>
      </xsd:element>
    </xsd:schema>
  </types>

  <message name="SubmitJobInput"><part name="parameters" element="tns:SubmitJob"/></message>
  <message name="SubmitJobOutput"><part name="parameters" element="tns:SubmitJobResponse"/></message>
  <message name="GetJobStatusInput"><part name="parameters" element="tns:GetJobStatus"/></message>
  <message name="GetJobStatusOutput"><part name="parameters" element="tns:GetJobStatusResponse"/></message>

  <portType name="JobPortType">
    <operation name="SubmitJob">
      <input message="tns:SubmitJobInput"/>
      <output message="tns:SubmitJobOutput"/>
    </operation>
    <operation name="GetJobStatus">
      <input message="tns:GetJobStatusInput"/>
      <output message="tns:GetJobStatusOutput"/>
    </operation>
  </portType>

  <binding name="JobBinding" type="tns:JobPortType">
    <soap:binding style="document" transport="http://schemas.xmlsoap.org/soap/http"/>
    <operation name="SubmitJob">
      <soap:operation soapAction="urn:httpserver:jobs#SubmitJob"/>
      <input><soap:body use="literal"/></input>
      <output><soap:body use="literal"/></output>
    </operation>
    <operation name="GetJobStatus">
      <soap:operation soapAction="urn:httpserver:jobs#GetJobStatus"/>
      <input><soap:body use="literal"/></input>
      <output><soap:body use="literal"/></output>
    </operation>
  </binding>

  <service name="JobService">
    <port name="JobPort" binding="tns:JobBinding">
      <soap:address location=")" + xml::escape(endpoint_url) + R"("/>
    </port>
  </service>
</definitions>
)";
}
