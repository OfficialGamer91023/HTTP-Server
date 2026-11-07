// REST (JSON) and SOAP (XML) front ends over the same job queue.
#pragma once

#include <string>

#include "http.hpp"
#include "jobs.hpp"

// POST /api/jobs[?delay_ms=N]   body = text to analyse -> 202 + Location
// GET  /api/jobs/{id}           -> 200 job status/result as JSON
http::Response handle_jobs_api(const http::Request& req, JobQueue& jobs);

// GET  /api/health              -> 200 {"status":"ok",...}
http::Response handle_health(const http::Request& req, const JobQueue& jobs);

// GET  /soap?wsdl               -> WSDL 1.1 service description
// POST /soap                    -> SOAP 1.1 SubmitJob / GetJobStatus
http::Response handle_soap(const http::Request& req, JobQueue& jobs);

std::string build_wsdl(const std::string& endpoint_url);

std::string json_escape(const std::string& s);
