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


class VolumeTests(unittest.TestCase):

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict | None) -> None:
        body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        self.server.state.next_status = status
        self.server.state.next_body = body

    def test_volume_human(self) -> None:
        self._set_response(200, {
            "ok": True,
            "total_b": 8 * 1024 * 1024 * 1024,
            "free_b": 1 * 1024 * 1024 * 1024,
            "fs_type": "FAT32",
        })
        code, out, _err = _run_cli(["--host", self.server.host, "volume"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("FAT32", out)
        self.assertIn("GB", out)
        self.assertEqual(self.server.state.last_path, "/api/v1/volume")
        self.assertEqual(self.server.state.last_method, "GET")

    def test_volume_json(self) -> None:
        self._set_response(200, {"ok": True, "total_b": 1, "free_b": 1,
                                 "fs_type": "FAT16"})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "--json", "volume"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(json.loads(out)["fs_type"], "FAT16")

    def test_volume_503_busy(self) -> None:
        self._set_response(503, {"ok": False, "code": "busy",
                                 "message": "SD not mounted"})
        code, _out, err = _run_cli(["--host", self.server.host, "volume"])
        self.assertEqual(code, sidecart.EXIT_BUSY)
        self.assertIn("busy", err)


class LsTests(unittest.TestCase):

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict | None) -> None:
        body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        self.server.state.next_status = status
        self.server.state.next_body = body

    def test_ls_default_root(self) -> None:
        self._set_response(200, {
            "ok": True, "path": "/",
            "entries": [
                {"name": "FOO.TXT", "size": 1234, "is_dir": False,
                 "mtime": "2026-04-30T12:34:56"},
                {"name": "SUB", "size": 0, "is_dir": True,
                 "mtime": "2026-04-29T08:00:00"},
            ],
            "truncated": False,
        })
        code, out, _err = _run_cli(["--host", self.server.host, "ls"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("FOO.TXT", out)
        self.assertIn("SUB", out)
        self.assertIn("file", out)
        self.assertIn("dir", out)
        # Default path resolves to /api/v1/files?path=%2F or /api/v1/files?path=/.
        self.assertTrue(self.server.state.last_path.startswith("/api/v1/files"))
        self.assertIn("path=", self.server.state.last_path)

    def test_ls_explicit_path(self) -> None:
        self._set_response(200, {"ok": True, "path": "/sub",
                                 "entries": [], "truncated": False})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "ls", "/sub/foo bar"])
        self.assertEqual(code, sidecart.EXIT_OK)
        # Spaces must be URL-encoded.
        self.assertIn("foo%20bar", self.server.state.last_path)

    def test_ls_truncated_flag_shown(self) -> None:
        self._set_response(200, {
            "ok": True, "path": "/",
            "entries": [{"name": "A", "size": 0, "is_dir": False,
                         "mtime": None}],
            "truncated": True,
        })
        code, out, _err = _run_cli(["--host", self.server.host, "ls"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("truncated", out)

    def test_ls_404_maps_to_exit_3(self) -> None:
        self._set_response(404, {"ok": False, "code": "not_found",
                                 "message": "Path not found"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "ls", "/missing"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("not_found", err)

    def test_ls_listing_a_file_returns_422(self) -> None:
        self._set_response(422, {"ok": False, "code": "is_file",
                                 "message": "Path is a file"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "ls", "/foo.txt"])
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("is_file", err)


class FolderMutationTests(unittest.TestCase):
    """Folder create / delete / rename via mkdir / rmdir / mvdir."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict | None) -> None:
        body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        self.server.state.next_status = status
        self.server.state.next_body = body

    # mkdir ----------------------------------------------------------

    def test_mkdir_201(self) -> None:
        self._set_response(201, {"ok": True, "path": "/sub"})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "mkdir", "/sub"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path, "/api/v1/folders/sub")
        self.assertIn("/sub", out)

    def test_mkdir_409_conflict(self) -> None:
        self._set_response(409, {"ok": False, "code": "conflict",
                                 "message": "Folder already exists"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "mkdir", "/sub"])
        self.assertEqual(code, sidecart.EXIT_CONFLICT)
        self.assertIn("conflict", err)

    def test_mkdir_400_name_too_long(self) -> None:
        self._set_response(400, {"ok": False, "code": "name_too_long",
                                 "message": "Stem > 8 chars"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "mkdir", "/longerthan8"])
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("name_too_long", err)

    # rmdir ----------------------------------------------------------

    def test_rmdir_204(self) -> None:
        # 204 No Content — empty body, no JSON.
        self._set_response(204, None)
        code, out, _err = _run_cli(
            ["--host", self.server.host, "rmdir", "/sub"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "DELETE")
        self.assertEqual(self.server.state.last_path, "/api/v1/folders/sub")
        self.assertIn("ok", out)

    def test_rmdir_409_not_empty(self) -> None:
        self._set_response(409, {"ok": False, "code": "conflict",
                                 "message": "Folder is not empty"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "rmdir", "/sub"])
        self.assertEqual(code, sidecart.EXIT_CONFLICT)
        self.assertIn("not empty", err)

    def test_rmdir_404_is_file(self) -> None:
        self._set_response(404, {"ok": False, "code": "is_file",
                                 "message": "Path is a file"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "rmdir", "/foo.txt"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("is_file", err)

    # mvdir ----------------------------------------------------------

    def test_mvdir_200(self) -> None:
        self._set_response(200, {"ok": True, "from": "/old", "to": "/new"})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "mvdir", "/old", "/new"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/folders/old/rename")
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body, {"to": "/new"})
        self.assertEqual(
            self.server.state.last_headers.get("Content-Type"),
            "application/json")
        self.assertIn("/old -> /new", out)

    def test_mvdir_422_cycle(self) -> None:
        self._set_response(422, {"ok": False, "code": "unprocessable",
                                 "message": "Cannot rename into descendant"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "mvdir", "/foo", "/foo/bar"])
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("unprocessable", err)


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
