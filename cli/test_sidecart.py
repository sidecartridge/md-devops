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


class FileMutationTests(unittest.TestCase):
    """File delete / rename via rm / mv."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict | None) -> None:
        body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        self.server.state.next_status = status
        self.server.state.next_body = body

    # rm -------------------------------------------------------------

    def test_rm_204(self) -> None:
        self._set_response(204, None)
        code, out, _err = _run_cli(
            ["--host", self.server.host, "rm", "/foo.txt"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "DELETE")
        self.assertEqual(self.server.state.last_path, "/api/v1/files/foo.txt")
        self.assertIn("ok", out)

    def test_rm_404_is_directory(self) -> None:
        self._set_response(404, {"ok": False, "code": "is_directory",
                                 "message": "Path is a directory"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "rm", "/sub"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("is_directory", err)

    def test_rm_404_not_found(self) -> None:
        self._set_response(404, {"ok": False, "code": "not_found",
                                 "message": "File not found"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "rm", "/missing"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("not_found", err)

    # mv -------------------------------------------------------------

    def test_mv_200(self) -> None:
        self._set_response(200, {"ok": True, "from": "/old.txt",
                                 "to": "/new.txt"})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "mv", "/old.txt", "/new.txt"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/files/old.txt/rename")
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body, {"to": "/new.txt"})
        self.assertEqual(
            self.server.state.last_headers.get("Content-Type"),
            "application/json")
        self.assertIn("/old.txt -> /new.txt", out)

    def test_mv_409_conflict(self) -> None:
        self._set_response(409, {"ok": False, "code": "conflict",
                                 "message": "Target already exists"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "mv", "/a.txt", "/b.txt"])
        self.assertEqual(code, sidecart.EXIT_CONFLICT)
        self.assertIn("conflict", err)

    def test_mv_404_is_directory(self) -> None:
        self._set_response(404, {"ok": False, "code": "is_directory",
                                 "message": "Path is a directory"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "mv", "/sub", "/sub2"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("is_directory", err)


class GetTests(unittest.TestCase):
    """File download via the `get` subcommand."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)
        # Each test uses its own working directory under tmp.
        import tempfile
        self.tmp = tempfile.mkdtemp(prefix="sidecart-get-")
        self.cwd = os.getcwd()
        os.chdir(self.tmp)
        self.addCleanup(self._cleanup)

    def _cleanup(self) -> None:
        os.chdir(self.cwd)
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _set_octets(self, status: int, body: bytes,
                    extra_headers: dict[str, str] | None = None) -> None:
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/octet-stream"}
        if extra_headers:
            self.server.state.next_headers.update(extra_headers)

    def _set_json_error(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_get_full_download(self) -> None:
        payload = b"A" * 5000
        self._set_octets(200, payload)
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "-q", "get", "/foo.bin"])
        self.assertEqual(code, sidecart.EXIT_OK)
        with open("foo.bin", "rb") as f:
            self.assertEqual(f.read(), payload)
        self.assertEqual(self.server.state.last_method, "GET")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/files/foo.bin")

    def test_get_local_override(self) -> None:
        self._set_octets(200, b"hello")
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "-q", "get", "/foo.bin", "out.bin"])
        self.assertEqual(code, sidecart.EXIT_OK)
        with open("out.bin", "rb") as f:
            self.assertEqual(f.read(), b"hello")

    def test_get_resume_sends_range(self) -> None:
        # Partial file already on disk: 100 bytes.
        with open("foo.bin", "wb") as f:
            f.write(b"X" * 100)
        # Server responds 206 with the trailing slice.
        slice_data = b"Y" * 50
        self._set_octets(206, slice_data, {
            "Content-Range": "bytes 100-149/150"})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "-q", "get", "/foo.bin", "-r"])
        self.assertEqual(code, sidecart.EXIT_OK)
        # The CLI must have sent Range: bytes=100-.
        self.assertEqual(
            self.server.state.last_headers.get("Range"), "bytes=100-")
        # Combined file: 100 X's + 50 Y's.
        with open("foo.bin", "rb") as f:
            self.assertEqual(f.read(), b"X" * 100 + b"Y" * 50)

    def test_get_404(self) -> None:
        self._set_json_error(404, {"ok": False, "code": "not_found",
                                   "message": "File not found"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "-q", "get", "/missing"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("not_found", err)

    def test_get_416_range_invalid(self) -> None:
        # Resume request, but server says range is invalid.
        with open("foo.bin", "wb") as f:
            f.write(b"X" * 200)
        self._set_json_error(416, {"ok": False, "code": "range_invalid",
                                   "message": "Range outside file"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "-q", "get", "/foo.bin", "-r"])
        # 416 falls into the "other 4xx" bucket = EXIT_BAD_REQUEST.
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("range_invalid", err)


class PutTests(unittest.TestCase):
    """File upload via the `put` subcommand."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)
        import tempfile
        self.tmp = tempfile.mkdtemp(prefix="sidecart-put-")
        self.cwd = os.getcwd()
        os.chdir(self.tmp)
        self.addCleanup(self._cleanup)

    def _cleanup(self) -> None:
        os.chdir(self.cwd)
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _set_response(self, status: int, payload: dict | None) -> None:
        body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def _write_local(self, name: str, data: bytes) -> None:
        with open(name, "wb") as f:
            f.write(data)

    def test_put_201_create(self) -> None:
        payload = b"\x01\x02\x03" * 1000  # 3 KB
        self._write_local("up.bin", payload)
        self._set_response(201, {"ok": True, "path": "/up.bin",
                                 "size": len(payload)})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "-q", "put", "up.bin"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "PUT")
        self.assertEqual(self.server.state.last_path, "/api/v1/files/up.bin")
        self.assertEqual(
            self.server.state.last_headers.get("Content-Type"),
            "application/octet-stream")
        self.assertEqual(
            int(self.server.state.last_headers.get("Content-Length", "0")),
            len(payload))
        self.assertEqual(self.server.state.last_body, payload)

    def test_put_force_adds_overwrite_query(self) -> None:
        self._write_local("a.txt", b"hello")
        self._set_response(200, {"ok": True, "path": "/a.txt", "size": 5})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "-q", "put", "a.txt", "/a.txt", "-f"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("overwrite=1", self.server.state.last_path)
        self.assertEqual(self.server.state.last_body, b"hello")

    def test_put_409_conflict_no_force(self) -> None:
        self._write_local("a.txt", b"hello")
        self._set_response(409, {"ok": False, "code": "conflict",
                                 "message": "File exists"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "-q", "put", "a.txt"])
        self.assertEqual(code, sidecart.EXIT_CONFLICT)
        self.assertIn("conflict", err)
        # No overwrite query when --force is absent.
        self.assertNotIn("overwrite=", self.server.state.last_path)

    def test_put_503_busy(self) -> None:
        self._write_local("a.txt", b"hello")
        self._set_response(503, {"ok": False, "code": "busy",
                                 "message": "Another upload in progress"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "-q", "put", "a.txt", "/a.txt", "-f"])
        self.assertEqual(code, sidecart.EXIT_BUSY)
        self.assertIn("busy", err)


class RunnerStatusTests(unittest.TestCase):
    """Epic 03 / S1 — `sidecart runner status`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_status_inactive(self) -> None:
        self._set_response(200, {
            "ok": True, "active": False, "busy": False, "cwd": "",
            "last_command": None, "last_path": None,
            "last_exit_code": None, "last_started_at_ms": None,
            "last_finished_at_ms": None,
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "GET")
        self.assertEqual(self.server.state.last_path, "/api/v1/runner")
        self.assertIn("not active", out)

    def test_runner_status_active(self) -> None:
        self._set_response(200, {
            "ok": True, "active": True, "busy": False, "cwd": "/games",
            "last_command": "EXECUTE", "last_path": "/games/prog.tos",
            "last_exit_code": 0, "last_started_at_ms": 1,
            "last_finished_at_ms": 2,
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("active   : true", out)
        self.assertIn("busy     : no", out)
        self.assertIn("/games", out)
        self.assertIn("EXECUTE", out)

    def test_runner_status_json(self) -> None:
        payload = {"ok": True, "active": True, "busy": False, "cwd": "",
                   "last_command": None, "last_path": None,
                   "last_exit_code": None, "last_started_at_ms": None,
                   "last_finished_at_ms": None}
        self._set_response(200, payload)
        code, out, _err = _run_cli(
            ["--host", self.server.host, "--json", "runner", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(json.loads(out)["active"], True)


class RunnerResetTests(unittest.TestCase):
    """Epic 03 / S2 — `sidecart runner reset`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_reset_202(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "reset"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path, "/api/v1/runner/reset")
        self.assertIn("RESET sent", out)

    def test_runner_reset_409_runner_inactive(self) -> None:
        self._set_response(409, {"ok": False, "code": "runner_inactive",
                                 "message": "Runner mode is not active"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "reset"])
        self.assertEqual(code, sidecart.EXIT_CONFLICT)
        self.assertIn("runner_inactive", err)


class RunnerRunTests(unittest.TestCase):
    """Epic 03 / S3 — `sidecart runner run`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_run_202_no_cmdline(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "run", "/PROG.TOS"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/runner/run")
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body["path"], "/PROG.TOS")
        self.assertEqual(body["cmdline"], "")
        self.assertIn("EXECUTE", out)

    def test_runner_run_202_with_cmdline(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "runner", "run",
             "/PROG.TOS", "-v", "--file", "foo.txt"])
        self.assertEqual(code, sidecart.EXIT_OK)
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body["cmdline"], "-v --file foo.txt")

    def test_runner_run_503_busy(self) -> None:
        self._set_response(503, {"ok": False, "code": "busy",
                                 "message": "Runner busy"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "run", "/PROG.TOS"])
        self.assertEqual(code, sidecart.EXIT_BUSY)
        self.assertIn("busy", err)

    def test_runner_run_404_not_found(self) -> None:
        self._set_response(404, {"ok": False, "code": "not_found",
                                 "message": "Program file not found"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "run", "/MISSING.TOS"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("not_found", err)


class RunnerCdTests(unittest.TestCase):
    """Epic 03 / S4 — `sidecart runner cd`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_cd_202(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "cd", "/GAMES"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/runner/cd")
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body["path"], "/GAMES")
        self.assertNotIn("cmdline", body)
        self.assertIn("CD", out)

    def test_runner_cd_404_not_found(self) -> None:
        self._set_response(404, {"ok": False, "code": "not_found",
                                 "message": "Directory not found"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "cd", "/NOPE"])
        self.assertEqual(code, sidecart.EXIT_NOT_FOUND)
        self.assertIn("not_found", err)

    def test_runner_cd_400_not_a_directory(self) -> None:
        self._set_response(400, {"ok": False, "code": "bad_path",
                                 "message": "Path is not a directory"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "cd", "/PROG.TOS"])
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("bad_path", err)

    def test_runner_cd_409_runner_inactive(self) -> None:
        self._set_response(409, {"ok": False, "code": "runner_inactive",
                                 "message": "Runner not active"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "cd", "/GAMES"])
        self.assertIn("runner_inactive", err)


class RunnerResTests(unittest.TestCase):
    """Epic 03 / S5 — `sidecart runner res`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_res_low_202(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "res", "low"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path, "/api/v1/runner/res")
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body, {"rez": "low"})
        self.assertIn("RES", out)

    def test_runner_res_med_202(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "runner", "res", "med"])
        self.assertEqual(code, sidecart.EXIT_OK)
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body, {"rez": "med"})

    def test_runner_res_rejects_unknown_rez(self) -> None:
        # argparse `choices` rejects before the request leaves the CLI.
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "res", "high"])
        self.assertNotEqual(code, sidecart.EXIT_OK)
        self.assertIn("invalid choice", err)

    def test_runner_res_409_runner_inactive(self) -> None:
        self._set_response(409, {"ok": False, "code": "runner_inactive",
                                 "message": "Runner not active"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "res", "low"])
        self.assertIn("runner_inactive", err)


class RunnerMeminfoTests(unittest.TestCase):
    """Epic 03 / S6 — `sidecart runner meminfo`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_meminfo_human(self) -> None:
        self._set_response(200, {
            "ok": True,
            "membottom": 0x000900,
            "memtop": 0x100000,
            "phystop": 0x100000,
            "screenmem": 0x0F8000,
            "basepage": 0x000C00,
            "bank0_kb": 512,
            "bank1_kb": 512,
            "decoded": True,
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "meminfo"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "GET")
        self.assertEqual(self.server.state.last_path, "/api/v1/runner/meminfo")
        self.assertIn("0x00100000", out)
        self.assertIn("512 KB", out)
        self.assertIn("total RAM         : 1024 KB", out)
        self.assertIn("[$432]", out)
        self.assertIn("[$FF8001 nibble]", out)

    def test_runner_meminfo_unknown_mmu(self) -> None:
        self._set_response(200, {
            "ok": True,
            "membottom": 0, "memtop": 0, "phystop": 0,
            "screenmem": 0, "basepage": 0,
            "bank0_kb": 0, "bank1_kb": 0,
            "decoded": False,
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "meminfo"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("unrecognised MMU config", out)

    def test_runner_meminfo_json(self) -> None:
        self._set_response(200, {
            "ok": True, "membottom": 0x900, "memtop": 0x100000,
            "phystop": 0x100000, "screenmem": 0xF8000, "basepage": 0xC00,
            "bank0_kb": 512, "bank1_kb": 512, "decoded": True,
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "--json", "runner", "meminfo"])
        self.assertEqual(code, sidecart.EXIT_OK)
        parsed = json.loads(out)
        self.assertEqual(parsed["bank0_kb"], 512)

    def test_runner_meminfo_504_timeout(self) -> None:
        self._set_response(504, {"ok": False, "code": "gateway_timeout",
                                 "message": "Runner did not respond"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "meminfo"])
        self.assertNotEqual(code, sidecart.EXIT_OK)
        self.assertIn("gateway_timeout", err)

    def test_runner_meminfo_409_runner_inactive(self) -> None:
        self._set_response(409, {"ok": False, "code": "runner_inactive",
                                 "message": "Runner not active"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "meminfo"])
        self.assertIn("runner_inactive", err)


class RunnerAdvStatusTests(unittest.TestCase):
    """Epic 04 / S1 — `sidecart runner adv status`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_adv_status_installed_etv(self) -> None:
        self._set_response(200, {
            "ok": True, "active": True, "installed": True,
            "hook_vector": "etv_timer",
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "GET")
        self.assertEqual(self.server.state.last_path, "/api/v1/runner/adv")
        self.assertIn("hook vector   : installed (etv_timer @ $400)", out)
        self.assertIn("runner active : yes", out)

    def test_runner_adv_status_installed_vbl(self) -> None:
        self._set_response(200, {
            "ok": True, "active": True, "installed": True,
            "hook_vector": "vbl",
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("hook vector   : installed (vbl @ $70)", out)

    def test_runner_adv_status_inactive(self) -> None:
        self._set_response(200, {
            "ok": True, "active": False, "installed": False,
            "hook_vector": "unknown",
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertIn("hook vector   : not installed", out)
        self.assertIn("runner active : no", out)

    def test_runner_adv_status_json(self) -> None:
        self._set_response(200, {
            "ok": True, "active": True, "installed": True,
            "hook_vector": "etv_timer",
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "--json",
             "runner", "adv", "status"])
        self.assertEqual(code, sidecart.EXIT_OK)
        parsed = json.loads(out)
        self.assertTrue(parsed["installed"])
        self.assertEqual(parsed["hook_vector"], "etv_timer")


class RunnerAdvMeminfoTests(unittest.TestCase):
    """Epic 04 / S6 — `sidecart runner adv meminfo`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_adv_meminfo_human(self) -> None:
        self._set_response(200, {
            "ok": True,
            "membottom": 0x000900,
            "memtop": 0x100000,
            "phystop": 0x100000,
            "screenmem": 0x0F8000,
            "basepage": 0x000C00,
            "bank0_kb": 512,
            "bank1_kb": 512,
            "decoded": True,
        })
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "meminfo"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/runner/adv/meminfo")
        self.assertIn("0x00100000", out)
        self.assertIn("total RAM         : 1024 KB", out)
        self.assertIn("[$432]", out)

    def test_runner_adv_meminfo_504_timeout(self) -> None:
        self._set_response(504, {"ok": False, "code": "gateway_timeout",
                                 "message": "Runner did not respond"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "meminfo"])
        self.assertNotEqual(code, sidecart.EXIT_OK)
        self.assertIn("gateway_timeout", err)

    def test_runner_adv_meminfo_409_runner_inactive(self) -> None:
        self._set_response(409, {"ok": False, "code": "runner_inactive",
                                 "message": "Runner not active"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "meminfo"])
        self.assertIn("runner_inactive", err)


class RunnerAdvJumpTests(unittest.TestCase):
    """Epic 04 / S7 — `sidecart runner adv jump`."""

    def setUp(self) -> None:
        self.server = _FakeServer()
        self.addCleanup(self.server.close)

    def _set_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.server.state.next_status = status
        self.server.state.next_body = body
        self.server.state.next_headers = {
            "Content-Type": "application/json"}

    def test_runner_adv_jump_decimal(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "16384"])
        self.assertEqual(code, sidecart.EXIT_OK)
        self.assertEqual(self.server.state.last_method, "POST")
        self.assertEqual(self.server.state.last_path,
                         "/api/v1/runner/adv/jump")
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        # CLI normalises to 0x-hex regardless of input form.
        self.assertEqual(body["address"], "0x4000")
        self.assertIn("ADV JUMP 0x004000", out)

    def test_runner_adv_jump_legacy_hex(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "$FA1C00"])
        self.assertEqual(code, sidecart.EXIT_OK)
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body["address"], "0xFA1C00")

    def test_runner_adv_jump_modern_hex(self) -> None:
        self._set_response(202, {"ok": True, "accepted": True})
        code, _out, _err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "0xfa1c00"])
        self.assertEqual(code, sidecart.EXIT_OK)
        body = json.loads(self.server.state.last_body.decode("utf-8"))
        self.assertEqual(body["address"], "0xFA1C00")

    def test_runner_adv_jump_rejects_odd(self) -> None:
        # Should not even reach the server — CLI validates first.
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "0x12345"])
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("odd", err)
        self.assertIsNone(self.server.state.last_method)

    def test_runner_adv_jump_rejects_out_of_range(self) -> None:
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "0x10000000"])
        self.assertEqual(code, sidecart.EXIT_BAD_REQUEST)
        self.assertIn("24-bit", err)
        self.assertIsNone(self.server.state.last_method)

    def test_runner_adv_jump_409_wrong_hook(self) -> None:
        self._set_response(409, {"ok": False, "code": "wrong_hook",
                                 "message": "VBL hook required"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "0xFA1C00"])
        self.assertIn("wrong_hook", err)

    def test_runner_adv_jump_409_runner_inactive(self) -> None:
        self._set_response(409, {"ok": False, "code": "runner_inactive",
                                 "message": "Runner not active"})
        code, _out, err = _run_cli(
            ["--host", self.server.host, "runner", "adv", "jump",
             "0xFA1C00"])
        self.assertIn("runner_inactive", err)


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
