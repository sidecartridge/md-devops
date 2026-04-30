"""
Unit tests for cli/sidecart.py.

Spin up an in-process http.server-based fake on 127.0.0.1:0 that
records the incoming verb / path / headers / body and serves canned
responses, then exercise the CLI by calling sidecart.main(argv) and
asserting on stdout / stderr / exit code.

Run:
    python3 -m unittest cli/test_sidecart.py
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(__file__))
import sidecart  # noqa: E402


class _FakeServerState:
    """Captures the last request and the canned response to serve."""

    def __init__(self) -> None:
        self.last_method: str | None = None
        self.last_path: str | None = None
        self.last_headers: dict[str, str] = {}
        self.last_body: bytes = b""
        # Canned response: (status, body_bytes, headers_dict)
        self.next_status = 200
        self.next_body = b""
        self.next_headers: dict[str, str] = {"Content-Type": "application/json"}


class _FakeHandler(BaseHTTPRequestHandler):
    """Records what the CLI sent and replies with whatever the test set up."""

    state: _FakeServerState  # injected per-test

    def log_message(self, format: str, *args) -> None:  # noqa: A002
        # Silence the default stderr access log so test output stays clean.
        return

    def _capture(self, method: str) -> None:
        s = self.state
        s.last_method = method
        s.last_path = self.path
        s.last_headers = dict(self.headers.items())
        length = int(self.headers.get("Content-Length", "0") or "0")
        s.last_body = self.rfile.read(length) if length > 0 else b""

    def _reply(self) -> None:
        s = self.state
        self.send_response(s.next_status)
        for name, value in s.next_headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(s.next_body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if s.next_body:
            self.wfile.write(s.next_body)

    def do_GET(self) -> None:
        self._capture("GET")
        self._reply()

    def do_HEAD(self) -> None:
        self._capture("HEAD")
        s = self.state
        self.send_response(s.next_status)
        for name, value in s.next_headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(s.next_body)))
        self.send_header("Connection", "close")
        self.end_headers()

    def do_POST(self) -> None:
        self._capture("POST")
        self._reply()

    def do_PUT(self) -> None:
        self._capture("PUT")
        self._reply()

    def do_DELETE(self) -> None:
        self._capture("DELETE")
        self._reply()


class _FakeServer:
    """Owns an HTTPServer running on its own thread, scoped to one test."""

    def __init__(self) -> None:
        self.state = _FakeServerState()
        # Bind a fresh handler subclass per server so the state injection
        # is per-instance rather than shared global mutable state.
        handler_cls = type(
            "BoundFakeHandler", (_FakeHandler,), {"state": self.state})
        self.httpd = HTTPServer(("127.0.0.1", 0), handler_cls)
        self.thread = threading.Thread(
            target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @property
    def host(self) -> str:
        host, port = self.httpd.server_address
        return f"{host}:{port}"

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)


def _run_cli(argv: list[str]) -> tuple[int, str, str]:
    """Invoke sidecart.main with patched stdout/stderr; return code+output."""
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = sidecart.main(argv)
        except SystemExit as exc:
            code = int(exc.code) if exc.code is not None else 0
    return code, out.getvalue(), err.getvalue()


class PingTests(unittest.TestCase):
    """End-to-end coverage of `sidecart ping` via the in-process fake."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict | None) -> None:
        body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        self.server.state.next_status = status
        self.server.state.next_body = body

    def test_ping_human_success(self) -> None:
        self._set_response(200, {"ok": True, "version": "v1.2.3", "uptime_s": 42})
        code, out, err = _run_cli(["--host", self.server.host, "ping"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("v1.2.3", out)
        self.assertIn("42s", out)
        self.assertEqual(err, "")
        self.assertEqual(self.server.state.last_method, "GET")
        self.assertEqual(self.server.state.last_path, "/api/v1/ping")
        self.assertIn("User-Agent", self.server.state.last_headers)

    def test_ping_json_success(self) -> None:
        self._set_response(200, {"ok": True, "version": "v9.9", "uptime_s": 1})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "--json", "ping"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(json.loads(out)["version"], "v9.9")

    def test_ping_quiet_success(self) -> None:
        self._set_response(200, {"ok": True, "version": "v0", "uptime_s": 0})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "-q", "ping"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(out, "")

    def test_ping_404_maps_to_exit_3(self) -> None:
        self._set_response(404, {"ok": False, "code": "not_found",
                                 "message": "Route not found"})
        code, out, err = _run_cli(["--host", self.server.host, "ping"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertEqual(out, "")
        self.assertIn("not_found", err)
        self.assertIn("404", err)

    def test_ping_503_maps_to_exit_6(self) -> None:
        self._set_response(503, {"ok": False, "code": "busy",
                                 "message": "SD not mounted"})
        code, _out, err = _run_cli(["--host", self.server.host, "ping"])
        self.assertEqual(code, sidecart.EXIT_BUSY)
        self.assertIn("busy", err)

    def test_ping_500_maps_to_exit_7(self) -> None:
        self._set_response(500, {"ok": False, "code": "internal_error",
                                 "message": "boom"})
        code, _out, err = _run_cli(["--host", self.server.host, "ping"])
        self.assertEqual(code, sidecart.EXIT_SERVER_ERROR)
        self.assertIn("internal_error", err)

    def test_ping_unreachable_maps_to_exit_8(self) -> None:
        # Use a port we know isn't listening (the server's port + 1 is
        # unlikely to be free but might be; safer to pick 1, which is
        # almost always closed).
        code, _out, err = _run_cli(
            ["--host", "127.0.0.1:1", "ping"])
        self.assertEqual(code, sidecart.EXIT_NETWORK)
        self.assertIn("cannot reach", err)


class HostResolutionTests(unittest.TestCase):

    def test_explicit_host_wins_over_env(self) -> None:
        os.environ["SIDECART_HOST"] = "envhost"
        try:
            self.assertEqual(sidecart.resolve_host("clihost"), "clihost")
        finally:
            del os.environ["SIDECART_HOST"]

    def test_env_used_when_no_cli(self) -> None:
        os.environ["SIDECART_HOST"] = "envhost"
        try:
            self.assertEqual(sidecart.resolve_host(None), "envhost")
        finally:
            del os.environ["SIDECART_HOST"]

    def test_default_when_neither(self) -> None:
        os.environ.pop("SIDECART_HOST", None)
        self.assertEqual(sidecart.resolve_host(None), sidecart.DEFAULT_HOST)


class BaseUrlTests(unittest.TestCase):

    def test_bare_hostname_gets_default_port(self) -> None:
        self.assertEqual(sidecart.base_url("foo.local"),
                         "http://foo.local:80")

    def test_host_with_port_kept(self) -> None:
        self.assertEqual(sidecart.base_url("foo.local:8080"),
                         "http://foo.local:8080")

    def test_full_url_kept(self) -> None:
        self.assertEqual(sidecart.base_url("http://foo.local"),
                         "http://foo.local")
        self.assertEqual(sidecart.base_url("http://foo.local/"),
                         "http://foo.local")


class StatusMappingTests(unittest.TestCase):

    def test_known_codes(self) -> None:
        self.assertEqual(sidecart.status_to_exit_code(200), sidecart.EXIT_OK)
        self.assertEqual(sidecart.status_to_exit_code(204), sidecart.EXIT_OK)
        self.assertEqual(sidecart.status_to_exit_code(206), sidecart.EXIT_OK)
        self.assertEqual(sidecart.status_to_exit_code(404),
                         sidecart.EXIT_NOT_FOUND)
        self.assertEqual(sidecart.status_to_exit_code(409),
                         sidecart.EXIT_CONFLICT)
        self.assertEqual(sidecart.status_to_exit_code(400),
                         sidecart.EXIT_BAD_REQUEST)
        self.assertEqual(sidecart.status_to_exit_code(422),
                         sidecart.EXIT_BAD_REQUEST)
        self.assertEqual(sidecart.status_to_exit_code(503), sidecart.EXIT_BUSY)
        self.assertEqual(sidecart.status_to_exit_code(500),
                         sidecart.EXIT_SERVER_ERROR)
        # Other 4xx falls through to bad_request.
        self.assertEqual(sidecart.status_to_exit_code(411),
                         sidecart.EXIT_BAD_REQUEST)


if __name__ == "__main__":
    unittest.main()
