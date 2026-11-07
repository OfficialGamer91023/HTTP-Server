#!/usr/bin/env python3
"""End-to-end tests: starts the real server binary and talks to it over TCP.

Usage: python3 tests/integration_test.py ./server
Uses only the standard library. If the optional `zeep` package is installed,
it also checks that a third-party SOAP client can consume the WSDL.
"""

import http.client
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./server"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "public")

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  ({detail})"))
    if not cond:
        failures.append(name)


def start_server():
    proc = subprocess.Popen(
        [BINARY, "--port", "0", "--root", ROOT, "--threads", "4"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    line = proc.stdout.readline()
    match = re.search(r":(\d+) ", line)
    if not match:
        proc.kill()
        sys.exit(f"server did not start: {line!r}")
    # Drain the access log in the background so the pipe never fills up.
    threading.Thread(target=lambda: proc.stdout.read(), daemon=True).start()
    return proc, int(match.group(1))


def raw(port, data, timeout=3.0):
    """Sends raw bytes and returns everything the server sends back."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(data)
        chunks = []
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        except socket.timeout:
            pass
        return b"".join(chunks)


def request(port, method, path, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp, data


SOAP_NS = "http://schemas.xmlsoap.org/soap/envelope/"


def soap(port, op_xml):
    envelope = (f'<?xml version="1.0"?><soap:Envelope xmlns:soap="{SOAP_NS}" '
                f'xmlns:j="urn:httpserver:jobs"><soap:Body>{op_xml}</soap:Body></soap:Envelope>')
    return request(port, "POST", "/soap", envelope.encode(),
                   {"Content-Type": "text/xml; charset=utf-8"})


def main():
    proc, port = start_server()
    try:
        run(port)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
            check("server shuts down cleanly on SIGTERM", proc.returncode == 0, proc.returncode)
        except subprocess.TimeoutExpired:
            proc.kill()
            check("server shuts down cleanly on SIGTERM", False, "timed out")

    print(f"\n{len(failures)} failure(s)" if failures else "\nall integration tests passed")
    sys.exit(1 if failures else 0)


def run(port):
    # --- static files and status codes ---
    resp, body = request(port, "GET", "/")
    check("GET / serves index.html", resp.status == 200 and b"http-server is running" in body)
    check("HTML content type", resp.getheader("Content-Type", "").startswith("text/html"))

    resp, body = request(port, "GET", "/style.css")
    check("CSS content type", resp.getheader("Content-Type", "").startswith("text/css"))

    resp, body = request(port, "HEAD", "/")
    check("HEAD has length but no body",
          resp.status == 200 and body == b"" and int(resp.getheader("Content-Length")) > 0)

    resp, _ = request(port, "GET", "/nope.html")
    check("missing file is 404 (server keeps running)", resp.status == 404)

    resp, _ = request(port, "DELETE", "/")
    check("unsupported method is 405 with Allow", resp.status == 405 and resp.getheader("Allow") == "GET, HEAD")

    out = raw(port, b"GET /../Makefile HTTP/1.1\r\nHost: x\r\n\r\n")
    check("path traversal blocked", out.startswith(b"HTTP/1.1 404"), out[:40])
    out = raw(port, b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n")
    check("encoded traversal blocked", out.startswith(b"HTTP/1.1 404") and b"root:" not in out, out[:40])

    out = raw(port, b"garbage\r\n\r\n")
    check("malformed request is 400", out.startswith(b"HTTP/1.1 400"), out[:40])
    out = raw(port, b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999\r\n\r\n")
    check("oversized body is 413", out.startswith(b"HTTP/1.1 413"), out[:40])
    out = raw(port, b"GET / HTTP/1.1\r\nHost: x\r\nX: " + b"a" * 10000 + b"\r\n\r\n")
    check("oversized headers are 431", out.startswith(b"HTTP/1.1 431"), out[:40])

    # --- keep-alive and pipelining ---
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    statuses = []
    for _ in range(3):
        conn.request("GET", "/api/health")
        r = conn.getresponse()
        r.read()
        statuses.append(r.status)
    conn.close()
    check("three requests on one keep-alive connection", statuses == [200, 200, 200], statuses)

    out = raw(port, b"GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n"
                    b"GET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    check("pipelined requests answered in order", out.count(b"HTTP/1.1 200 OK") == 2 and
          out.index(b"pending_jobs") < out.index(b"font-family"))

    out = raw(port, b"GET / HTTP/1.0\r\n\r\n")
    check("HTTP/1.0 closes by default", b"Connection: close" in out)

    # --- concurrency: a stalled client must not block others ---
    stalled = socket.create_connection(("127.0.0.1", port))
    stalled.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")  # never finishes its headers
    start = time.time()
    resp, _ = request(port, "GET", "/api/health")
    check("slow client does not block other clients", resp.status == 200 and time.time() - start < 1.0)
    stalled.close()

    results = []
    lock = threading.Lock()

    def worker():
        try:
            r, _ = request(port, "GET", "/")
            with lock:
                results.append(r.status)
        except Exception as e:  # noqa: BLE001
            with lock:
                results.append(str(e))

    threads = [threading.Thread(target=worker) for _ in range(100)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check("100 concurrent clients all get 200", results.count(200) == 100, set(map(str, results)))

    # --- async jobs over REST ---
    start = time.time()
    resp, body = request(port, "POST", "/api/jobs?delay_ms=500", b"the quick brown fox\njumps\n")
    submit_time = time.time() - start
    job = json.loads(body)
    check("POST /api/jobs returns 202 immediately", resp.status == 202 and submit_time < 0.3, submit_time)
    check("202 has Location header", resp.getheader("Location") == f"/api/jobs/{job['id']}")

    _, body = request(port, "GET", f"/api/jobs/{job['id']}")
    check("job is not done before its delay", json.loads(body)["status"] in ("queued", "running"))

    status = None
    for _ in range(50):
        _, body = request(port, "GET", f"/api/jobs/{job['id']}")
        status = json.loads(body)
        if status["status"] == "done":
            break
        time.sleep(0.05)
    check("job completes with correct counts",
          status["status"] == "done" and status["result"] == {"lines": 2, "words": 5, "bytes": 26}, status)

    resp, _ = request(port, "GET", "/api/jobs/123456")
    check("unknown job is 404", resp.status == 404)

    # --- SOAP ---
    resp, body = request(port, "GET", "/soap?wsdl")
    check("WSDL served", resp.status == 200 and b"<definitions" in body)

    resp, body = soap(port, "<j:SubmitJob><j:text>a &lt;b&gt; c</j:text></j:SubmitJob>")
    m = re.search(rb"<tns:jobId>(\d+)</tns:jobId>", body)
    check("SOAP SubmitJob returns a job id", resp.status == 200 and m is not None, body[:200])
    if m:
        time.sleep(0.2)
        resp, body = soap(port, f"<j:GetJobStatus><j:jobId>{int(m.group(1))}</j:jobId></j:GetJobStatus>")
        check("SOAP GetJobStatus returns results", b"<tns:status>done</tns:status>" in body and
              b"<tns:words>3</tns:words>" in body, body[:300])

    resp, body = soap(port, "<j:SubmitJob><j:text>unclosed</j:SubmitJob>")
    check("malformed SOAP returns a Client fault with 500", resp.status == 500 and b"soap:Client" in body)

    resp, body = request(port, "POST", "/soap",
                         b'<!DOCTYPE x [<!ENTITY a "aaaa">]><x>&a;</x>', {"Content-Type": "text/xml"})
    check("DOCTYPE (entity expansion / XXE) rejected", resp.status == 500 and b"DOCTYPE" in body)

    try:
        import zeep  # optional
    except ImportError:
        print("SKIP zeep interop test (pip install zeep to enable)")
        return
    client = zeep.Client(f"http://127.0.0.1:{port}/soap?wsdl")
    result = client.service.SubmitJob(text="interop works fine")
    time.sleep(0.2)
    status = client.service.GetJobStatus(jobId=result.jobId)
    check("zeep (third-party SOAP client) can call the service",
          status.status == "done" and status.words == 3, status)


if __name__ == "__main__":
    main()
