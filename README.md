# http-server

A multithreaded HTTP/1.1 server written from scratch in C++17 on raw POSIX sockets, with no external libraries.
It serves static files, and exposes one small background-job service through two interfaces: a REST/JSON API and a SOAP 1.1 web service with a WSDL.

I wrote it to understand what web frameworks and integration platforms do underneath: how requests are framed on a TCP stream, how one server handles many clients at once, and how synchronous and asynchronous APIs differ.

## Features

**HTTP/1.1**
- Incremental request parser: handles requests split across many TCP reads and several pipelined requests in a single read
- Case-insensitive headers, `Content-Length` bodies, percent-decoded paths, query strings
- Correct status codes: 200, 202, 400, 404, 405 (with `Allow`), 408, 413, 415, 431, 500, 501, 503, 505
- Persistent connections (keep-alive) with an idle timeout and a per-connection request cap; HTTP/1.0 closes by default
- `GET` and `HEAD` for static files, with MIME types for HTML, CSS, JS, JSON, images, PDF and more

**Concurrency**
- Fixed-size thread pool with a bounded queue; when it is full the server answers `503 Retry-After` instead of piling up work
- A slow or stalled client ties up one worker, not the whole server
- Clean shutdown on `SIGINT`/`SIGTERM`

**Async job queue (REST, JSON)**
- `POST /api/jobs` accepts text and returns `202 Accepted` at once with a `Location` header
- Background workers process the job (count lines, words and bytes, like `wc`); an optional `delay_ms` simulates slow work
- Clients poll `GET /api/jobs/{id}` until the status is `done`

**SOAP 1.1 web service (XML)**
- `GET /soap?wsdl` returns a WSDL 1.1 description (document/literal)
- `POST /soap` handles `SubmitJob` and `GetJobStatus` against the same queue the REST API uses
- Errors come back as SOAP faults (`soap:Client`, `soap:Server`, `soap:VersionMismatch`) with HTTP 500, per the SOAP 1.1 spec
- Small strict XML parser written for this project: namespaces, entities, CDATA, comments

**Safety**
- Path traversal blocked, including `%2e%2e` encodings and symlinks that point outside the document root
- Limits on header size (8 KB) and body size (1 MB)
- Duplicate `Content-Length` / `Host` headers and `Transfer-Encoding` are rejected, closing request-smuggling gaps
- XML `DOCTYPE` is refused, which rules out entity-expansion ("billion laughs") and XXE attacks

## Build and run

Needs a C++17 compiler, `make`, and Linux or macOS.

```bash
make
./server --port 8080 --root public
```

Options: `--port N` (0 picks a free port), `--bind ADDR` (default `127.0.0.1`), `--root DIR`, `--threads N` (default 8), `--job-workers N` (default 2).

## Try it

```bash
# Static files
curl -i http://localhost:8080/

# Async job over REST
curl -i -X POST --data-binary 'the quick brown fox' 'http://localhost:8080/api/jobs?delay_ms=2000'
# HTTP/1.1 202 Accepted
# Location: /api/jobs/1
curl http://localhost:8080/api/jobs/1
# {"id":1,"status":"running"}   ... then after 2s:
# {"id":1,"status":"done","result":{"lines":0,"words":4,"bytes":19}}

# The same job over SOAP
curl http://localhost:8080/soap?wsdl
curl -X POST http://localhost:8080/soap \
  -H 'Content-Type: text/xml; charset=utf-8' \
  -H 'SOAPAction: "urn:httpserver:jobs#SubmitJob"' \
  --data '<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/" xmlns:j="urn:httpserver:jobs">
            <soap:Body><j:SubmitJob><j:text>hello soap world</j:text></j:SubmitJob></soap:Body>
          </soap:Envelope>'
```

Any SOAP client can import the WSDL. The integration tests check this with [zeep](https://docs.python-zeep.org/) (Python); SoapUI works the same way.

## Tests

```bash
make test       # unit tests + end-to-end tests against the real binary
make sanitize   # the same, built with AddressSanitizer and UndefinedBehaviorSanitizer
```

- `tests/unit_tests.cpp`: 126 checks covering the parser, static file resolution, the XML parser, the job queue, and both APIs
- `tests/integration_test.py`: starts the server and checks real traffic: status codes, traversal attempts, keep-alive, pipelining, 100 concurrent clients, a stalled client, the async job flow, SOAP calls and faults, and interop with a third-party SOAP client

The suite also runs clean under ThreadSanitizer.

## Layout

```
src/http.*           request parser, response serializer
src/server.*         accept loop, keep-alive connection handling, routing
src/thread_pool.hpp  bounded worker pool
src/static_files.*   document root, path safety, MIME types
src/jobs.*           background job queue
src/api.*            REST and SOAP handlers, WSDL
src/xml.*            XML parser
tests/               unit and integration tests
public/              sample site
```

## Limits

This is a learning project, not a production server. It does not do TLS, HTTP/2, chunked request bodies, or IPv6, and it reads whole files into memory. Jobs live in memory, so they are lost on restart.

## History

v1 was a single-file, single-threaded server built for a university project that forwarded form posts to PHP. v2 is a full rewrite.
