#!/usr/bin/env python3
"""
sidecart — Python CLI for the md-devops Remote HTTP Management API.

Single-file, stdlib-only (Python >= 3.10). Talks HTTP to a Pico W
running the md-devops microfirmware. See docs/api.md for the full
endpoint reference.

Usage:
    python3 cli/sidecart.py [--host HOST[:PORT]] [--json] [-q] <verb> [args]

Default host is `sidecart.local` (mDNS); override with `--host` or
the `SIDECART_HOST` env var. `--json` emits the raw response envelope
on stdout instead of human-readable output. `-q/--quiet` silences
normal output; errors always go to stderr.

Subcommands:
    ping                                 GET  /api/v1/ping
    volume                               GET  /api/v1/volume
    ls [PATH]                            GET  /api/v1/files?path=...
    get REMOTE [LOCAL] [-r/--resume]     GET  /api/v1/files/<rel>
    put LOCAL [REMOTE] [-f/--force]      PUT  /api/v1/files/<rel>
    rm REMOTE                            DEL  /api/v1/files/<rel>
    mv FROM TO                           POST /api/v1/files/<from>/rename
    mkdir REMOTE                         POST /api/v1/folders/<rel>
    rmdir REMOTE                         DEL  /api/v1/folders/<rel>
    mvdir FROM TO                        POST /api/v1/folders/<from>/rename

Exit codes:
    0  success
    1  generic / unexpected
    2  argparse usage error
    3  server returned 404
    4  server returned 409
    5  server returned 400 / 422 / other 4xx
    6  server returned 503 (busy or SD unmounted)
    7  server returned 5xx other than 503
    8  network / DNS error (couldn't reach the host)

Examples:
    python3 cli/sidecart.py ping
    SIDECART_HOST=192.168.1.42 python3 cli/sidecart.py ls /games
    python3 cli/sidecart.py get FILE.TOS -r
    python3 cli/sidecart.py put LOCAL.PRG -f
    python3 cli/sidecart.py mvdir OLDNAME NEWNAME
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_HOST = "sidecart.local"
DEFAULT_PORT = 80
USER_AGENT = "sidecart-cli/0.1"
REQUEST_TIMEOUT_S = 10

# Granular exit-code map (see docs/epics/02-http-api.md "CLI / Exit code
# map"). Lets shell scripts branch on category.
EXIT_OK = 0
EXIT_GENERIC = 1
EXIT_USAGE = 2
EXIT_NOT_FOUND = 3
EXIT_CONFLICT = 4
EXIT_BAD_REQUEST = 5
EXIT_BUSY = 6
EXIT_SERVER_ERROR = 7
EXIT_NETWORK = 8


def resolve_host(cli_host: str | None) -> str:
    """Pick host:port to talk to.

    Precedence: explicit --host > $SIDECART_HOST > DEFAULT_HOST.
    """
    if cli_host:
        return cli_host
    env = os.environ.get("SIDECART_HOST", "").strip()
    if env:
        return env
    return DEFAULT_HOST


def base_url(host: str) -> str:
    if "://" in host:
        return host.rstrip("/")
    if ":" in host:
        return f"http://{host}"
    return f"http://{host}:{DEFAULT_PORT}"


def status_to_exit_code(status: int) -> int:
    """Map an HTTP status code from the server to a CLI exit code."""
    if 200 <= status < 300:
        return EXIT_OK
    if status == 404:
        return EXIT_NOT_FOUND
    if status == 409:
        return EXIT_CONFLICT
    if status in (400, 422):
        return EXIT_BAD_REQUEST
    if status == 503:
        return EXIT_BUSY
    if 500 <= status < 600:
        return EXIT_SERVER_ERROR
    # 4xx other than the buckets above (e.g. 405, 411, 413, 415, 416).
    return EXIT_BAD_REQUEST


def request_json(method: str, url: str, *, body: bytes | None = None,
                 headers: dict[str, str] | None = None,
                 timeout: float = REQUEST_TIMEOUT_S
                 ) -> tuple[int, dict | None, bytes]:
    """GET/HEAD/POST/PUT/DELETE the URL and return (status, parsed_json, raw).

    parsed_json is None when the response body isn't valid JSON or when
    the body is empty. The raw bytes are always returned so callers
    that want to handle non-JSON responses (file downloads in S5) can.

    HTTP error responses (4xx, 5xx) are NOT raised — they're returned
    with their status code so the caller can render the JSON error
    envelope. urllib.error.URLError (DNS / connection refused) is the
    only thing that aborts with EXIT_NETWORK.
    """
    req_headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=body, headers=req_headers,
                                 method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        # Server returned >= 400 — read its body so we can render the
        # error envelope, then close the response handle to silence
        # ResourceWarning under Python 3.14+.
        try:
            raw = exc.read() if exc.fp is not None else b""
        finally:
            exc.close()
        status = exc.code
    parsed: dict | None = None
    if raw:
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            parsed = None
    return status, parsed, raw


def render_error(parsed: dict | None, raw: bytes, status: int,
                 stream=None) -> None:
    """Render an error envelope on stderr in human form.

    `stream` resolves at call time (default sys.stderr) so callers that
    redirect sys.stderr (e.g. unittest harnesses) capture our output.
    """
    if stream is None:
        stream = sys.stderr
    if parsed and parsed.get("ok") is False:
        code = parsed.get("code", "unknown")
        msg = parsed.get("message", "")
        print(f"error ({status} {code}): {msg}", file=stream)
    else:
        body_preview = raw.decode("utf-8", errors="replace").strip()
        print(f"error ({status}): {body_preview}", file=stream)


def cmd_ping(args: argparse.Namespace) -> int:
    """GET /api/v1/ping → print version + uptime, or JSON with --json."""
    url = base_url(args.host) + "/api/v1/ping"
    try:
        status, parsed, raw = request_json("GET", url)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 200 or parsed is None or parsed.get("ok") is not True:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        # Re-emit with stable formatting.
        json.dump(parsed, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    elif not args.quiet:
        version = parsed.get("version", "?")
        uptime_s = parsed.get("uptime_s", 0)
        print(f"ok  version={version}  uptime={uptime_s}s")
    return EXIT_OK


def _format_bytes(n: int) -> str:
    """Render a byte count as a short human-readable string."""
    units = ("B", "KB", "MB", "GB", "TB")
    size = float(n)
    for unit in units:
        if size < 1024.0 or unit == units[-1]:
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} {unit}"
        size /= 1024.0
    return f"{n} B"  # unreachable


def cmd_volume(args: argparse.Namespace) -> int:
    """GET /api/v1/volume → print free / total / fs_type."""
    url = base_url(args.host) + "/api/v1/volume"
    try:
        status, parsed, raw = request_json("GET", url)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 200 or parsed is None or parsed.get("ok") is not True:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        json.dump(parsed, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    elif not args.quiet:
        total_b = int(parsed.get("total_b", 0))
        free_b = int(parsed.get("free_b", 0))
        fs_type = parsed.get("fs_type", "?")
        print(f"free {_format_bytes(free_b)} / "
              f"total {_format_bytes(total_b)}  ({fs_type})")
    return EXIT_OK


def _folders_url(host: str, remote: str) -> str:
    """Build /api/v1/folders/<remote>, URL-encoding everything except '/'."""
    encoded = urllib.parse.quote(remote.lstrip("/"), safe="/")
    return base_url(host) + "/api/v1/folders/" + encoded


def _files_url(host: str, remote: str) -> str:
    """Build /api/v1/files/<remote>, URL-encoding everything except '/'."""
    encoded = urllib.parse.quote(remote.lstrip("/"), safe="/")
    return base_url(host) + "/api/v1/files/" + encoded


def _do_mutation(args: argparse.Namespace, method: str, url: str,
                 body: bytes | None,
                 headers: dict[str, str] | None) -> int:
    """Shared scaffolding for write subcommands: send the request, map
    response to exit code, render JSON or human output."""
    try:
        status, parsed, raw = request_json(method, url, body=body,
                                           headers=headers)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    # Successful no-content responses (204 delete) return empty raw.
    success = (200 <= status < 300)
    if not success:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        if parsed is not None:
            json.dump(parsed, sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
        else:
            sys.stdout.write("{}\n")
    elif not args.quiet:
        if parsed and "path" in parsed:
            print(f"ok  {parsed['path']}")
        elif parsed and "from" in parsed and "to" in parsed:
            print(f"ok  {parsed['from']} -> {parsed['to']}")
        else:
            print("ok")
    return EXIT_OK


def cmd_mkdir(args: argparse.Namespace) -> int:
    """POST /api/v1/folders/<remote>."""
    url = _folders_url(args.host, args.remote)
    return _do_mutation(args, "POST", url, body=None, headers=None)


def cmd_rmdir(args: argparse.Namespace) -> int:
    """DELETE /api/v1/folders/<remote>."""
    url = _folders_url(args.host, args.remote)
    return _do_mutation(args, "DELETE", url, body=None, headers=None)


def cmd_mvdir(args: argparse.Namespace) -> int:
    """POST /api/v1/folders/<from>/rename body {'to': '<to>'}."""
    url = _folders_url(args.host, args.from_) + "/rename"
    body = json.dumps({"to": args.to}, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    return _do_mutation(args, "POST", url, body=body, headers=headers)


def cmd_rm(args: argparse.Namespace) -> int:
    """DELETE /api/v1/files/<remote>."""
    url = _files_url(args.host, args.remote)
    return _do_mutation(args, "DELETE", url, body=None, headers=None)


def cmd_mv(args: argparse.Namespace) -> int:
    """POST /api/v1/files/<from>/rename body {'to': '<to>'}."""
    url = _files_url(args.host, args.from_) + "/rename"
    body = json.dumps({"to": args.to}, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    return _do_mutation(args, "POST", url, body=body, headers=headers)


def _format_progress(done: int, total: int) -> str:
    if total > 0:
        return f"{done // 1024} KB / {total // 1024} KB"
    return f"{done // 1024} KB"


def cmd_get(args: argparse.Namespace) -> int:
    """GET /api/v1/files/<remote> → stream raw bytes to LOCAL."""
    url = _files_url(args.host, args.remote)
    local_path = args.local or os.path.basename(args.remote.rstrip("/"))
    if not local_path:
        print("error: cannot derive local filename from remote path",
              file=sys.stderr)
        return EXIT_USAGE

    resume_offset = 0
    if args.resume and os.path.exists(local_path):
        resume_offset = os.path.getsize(local_path)

    headers = {"User-Agent": USER_AGENT, "Accept": "application/octet-stream"}
    if resume_offset > 0:
        headers["Range"] = f"bytes={resume_offset}-"

    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
        resp = urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_S)
    except urllib.error.HTTPError as exc:
        try:
            raw = exc.read() if exc.fp is not None else b""
        finally:
            exc.close()
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            parsed = None
        render_error(parsed, raw, exc.code)
        return status_to_exit_code(exc.code)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    try:
        # Total size for progress: prefer Content-Range (Range responses)
        # else Content-Length.
        total = 0
        cr = resp.headers.get("Content-Range")
        if cr and "/" in cr:
            try:
                total = int(cr.rsplit("/", 1)[1])
            except ValueError:
                total = 0
        if not total:
            try:
                total = int(resp.headers.get("Content-Length", "0"))
                if resume_offset > 0:
                    total += resume_offset
            except ValueError:
                total = 0

        mode = "ab" if resume_offset > 0 and resp.status == 206 else "wb"
        # If we asked to resume but the server returned 200 (no Range
        # support / range ignored), restart from scratch.
        if mode == "ab" and resp.status != 206:
            mode = "wb"
            resume_offset = 0

        done = resume_offset
        chunk_size = 8192
        with open(local_path, mode) as out:
            while True:
                chunk = resp.read(chunk_size)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if not args.quiet:
                    sys.stderr.write(f"\r{_format_progress(done, total)}")
                    sys.stderr.flush()
        if not args.quiet:
            # Newline after the progress line.
            sys.stderr.write("\n")
            sys.stderr.flush()
    finally:
        resp.close()

    if args.json:
        json.dump({"ok": True, "path": args.remote, "local": local_path,
                   "size": done},
                  sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    elif not args.quiet:
        print(f"ok  {args.remote} -> {local_path}  ({done} bytes)")
    return EXIT_OK


def _split_host(host: str) -> tuple[str, int]:
    """Parse host[:port] or http://host[:port]/. Default port = 80."""
    if "://" in host:
        host = host.split("://", 1)[1]
    host = host.rstrip("/")
    if ":" in host:
        h, p = host.rsplit(":", 1)
        try:
            return h, int(p)
        except ValueError:
            return host, DEFAULT_PORT
    return host, DEFAULT_PORT


def cmd_runner_reset(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/reset — fire-and-forget cold reset."""
    url = base_url(args.host) + "/api/v1/runner/reset"
    try:
        status, parsed, raw = request_json("POST", url, body=b"")
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 202:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        if parsed is not None:
            json.dump(parsed, sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
    elif not args.quiet:
        print("ok  RESET sent")
    return EXIT_OK


def cmd_runner_status(args: argparse.Namespace) -> int:
    """GET /api/v1/runner — Epic 03 Runner state."""
    url = base_url(args.host) + "/api/v1/runner"
    try:
        status, parsed, raw = request_json("GET", url)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 200 or parsed is None or parsed.get("ok") is not True:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        json.dump(parsed, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
        return EXIT_OK
    if args.quiet:
        return EXIT_OK

    active = parsed.get("active", False)
    if not active:
        print("Runner mode is not active. Boot via [U] to enable.")
        return EXIT_OK

    busy = parsed.get("busy", False)
    cwd = parsed.get("cwd", "") or "(unset)"
    last_cmd = parsed.get("last_command")
    last_path = parsed.get("last_path")
    last_exit = parsed.get("last_exit_code")
    print(f"active   : true")
    print(f"busy     : {'yes' if busy else 'no'}")
    print(f"cwd      : {cwd}")
    if last_cmd is None:
        print(f"last     : (no command run yet)")
    elif last_path is None:
        print(f"last     : {last_cmd} (exit={last_exit})")
    else:
        print(f"last     : {last_cmd} {last_path} (exit={last_exit})")
    return EXIT_OK


def cmd_put(args: argparse.Namespace) -> int:
    """PUT /api/v1/files/<remote>?overwrite=0|1 — stream LOCAL up.

    Uses http.client.HTTPConnection so we can interleave a progress
    counter with each chunk send (urllib.request would buffer up the
    whole body before reading the response).
    """
    local = args.local
    if not os.path.isfile(local):
        print(f"error: local file not found: {local}", file=sys.stderr)
        return EXIT_USAGE
    remote = args.remote or os.path.basename(local)
    if not remote:
        print("error: cannot derive remote filename from local path",
              file=sys.stderr)
        return EXIT_USAGE

    size = os.path.getsize(local)
    encoded = urllib.parse.quote(remote.lstrip("/"), safe="/")
    path = "/api/v1/files/" + encoded
    if args.force:
        path += "?overwrite=1"

    host, port = _split_host(args.host)

    raw = b""
    status = 0
    sent = 0
    try:
        conn = http.client.HTTPConnection(host, port,
                                          timeout=REQUEST_TIMEOUT_S)
        try:
            conn.putrequest("PUT", path, skip_host=True,
                            skip_accept_encoding=True)
            conn.putheader("Host", f"{host}:{port}")
            conn.putheader("User-Agent", USER_AGENT)
            conn.putheader("Content-Type", "application/octet-stream")
            conn.putheader("Content-Length", str(size))
            conn.endheaders()

            with open(local, "rb") as f:
                while True:
                    chunk = f.read(8192)
                    if not chunk:
                        break
                    conn.send(chunk)
                    sent += len(chunk)
                    if not args.quiet:
                        sys.stderr.write(
                            f"\r{_format_progress(sent, size)}")
                        sys.stderr.flush()
            if not args.quiet:
                sys.stderr.write("\n")
                sys.stderr.flush()

            resp = conn.getresponse()
            try:
                raw = resp.read()
                status = resp.status
            finally:
                resp.close()
        finally:
            conn.close()
    except OSError as exc:
        print(f"error: cannot reach {host}:{port}: {exc}", file=sys.stderr)
        return EXIT_NETWORK

    parsed: dict | None = None
    if raw:
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            parsed = None

    success = (200 <= status < 300)
    if not success:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        if parsed is not None:
            json.dump(parsed, sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
    elif not args.quiet:
        target = parsed.get("path") if parsed else remote
        print(f"ok  {local} -> {target}  ({sent} bytes)")
    return EXIT_OK


def cmd_ls(args: argparse.Namespace) -> int:
    """GET /api/v1/files?path=PATH → print entries."""
    path = args.path or "/"
    encoded = urllib.parse.quote(path, safe="/")
    url = base_url(args.host) + f"/api/v1/files?path={encoded}"
    try:
        status, parsed, raw = request_json("GET", url)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 200 or parsed is None or parsed.get("ok") is not True:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        json.dump(parsed, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
        return EXIT_OK

    if args.quiet:
        return EXIT_OK

    entries = parsed.get("entries") or []
    # Column widths: name padded so dirs and sizes align.
    name_width = max((len(e.get("name", "")) for e in entries), default=4)
    name_width = max(name_width, 4)
    print(f"{'name'.ljust(name_width)}  {'size':>10}  type  mtime")
    for entry in entries:
        name = entry.get("name", "")
        size = entry.get("size", 0)
        is_dir = entry.get("is_dir", False)
        mtime = entry.get("mtime") or "-"
        kind = "dir" if is_dir else "file"
        print(f"{name.ljust(name_width)}  {size:>10}  {kind:<4}  {mtime}")
    if parsed.get("truncated"):
        print(f"({len(entries)} entries shown — listing truncated)")
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="sidecart",
        description="CLI for the md-devops Remote HTTP Management API.",
    )
    p.add_argument(
        "--host",
        default=None,
        help="Server host[:port] (default: $SIDECART_HOST or "
             f"{DEFAULT_HOST}).",
    )
    p.add_argument(
        "--json",
        action="store_true",
        help="Emit raw JSON instead of human-readable output.",
    )
    p.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Suppress normal output (errors still go to stderr).",
    )

    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping", help="Health check; show version and uptime.")
    sub.add_parser("volume", help="Show SD card total / free space.")
    ls = sub.add_parser("ls", help="List a folder on the SD card.")
    ls.add_argument("path", nargs="?", default="/",
                    help="Folder path (default: /).")
    mkdir = sub.add_parser("mkdir", help="Create a folder.")
    mkdir.add_argument("remote", help="Folder path on the SD card.")
    rmdir = sub.add_parser("rmdir", help="Delete an empty folder.")
    rmdir.add_argument("remote", help="Folder path on the SD card.")
    mvdir = sub.add_parser("mvdir", help="Rename or move a folder.")
    mvdir.add_argument("from_", metavar="FROM",
                       help="Source folder path.")
    mvdir.add_argument("to", help="Destination folder path.")
    rm = sub.add_parser("rm", help="Delete a file.")
    rm.add_argument("remote", help="File path on the SD card.")
    mv = sub.add_parser("mv", help="Rename or move a file.")
    mv.add_argument("from_", metavar="FROM", help="Source file path.")
    mv.add_argument("to", help="Destination file path.")
    get = sub.add_parser("get", help="Download a file.")
    get.add_argument("remote", help="File path on the SD card.")
    get.add_argument("local", nargs="?", default=None,
                     help="Local destination (default: basename of REMOTE).")
    get.add_argument("-r", "--resume", action="store_true",
                     help="Resume partial download via Range:.")
    put = sub.add_parser("put", help="Upload a file.")
    put.add_argument("local", help="Local file to upload.")
    put.add_argument("remote", nargs="?", default=None,
                     help="Remote destination (default: basename of LOCAL).")
    put.add_argument("-f", "--force", action="store_true",
                     help="Overwrite if the remote file exists.")

    runner = sub.add_parser("runner", help="Runner mode (Epic 03).")
    runner_sub = runner.add_subparsers(dest="runner_cmd", required=True)
    runner_sub.add_parser("status", help="Show Runner state and last completion.")
    runner_sub.add_parser("reset", help="Cold-reset the Atari ST.")
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.host = resolve_host(args.host)

    handlers = {
        "ping": cmd_ping,
        "volume": cmd_volume,
        "ls": cmd_ls,
        "mkdir": cmd_mkdir,
        "rmdir": cmd_rmdir,
        "mvdir": cmd_mvdir,
        "rm": cmd_rm,
        "mv": cmd_mv,
        "get": cmd_get,
        "put": cmd_put,
    }
    if args.cmd == "runner":
        runner_handlers = {
            "status": cmd_runner_status,
            "reset": cmd_runner_reset,
        }
        handler = runner_handlers.get(args.runner_cmd)
        if handler is None:
            parser.error(f"unknown runner subcommand: {args.runner_cmd}")
            return EXIT_USAGE
        return handler(args)
    handler = handlers.get(args.cmd)
    if handler is None:
        parser.error(f"unknown subcommand: {args.cmd}")
        return EXIT_USAGE
    return handler(args)


if __name__ == "__main__":
    sys.exit(main())
