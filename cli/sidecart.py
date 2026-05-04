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
    ping                                          GET  /api/v1/ping
    gemdrive volume                               GET  /api/v1/gemdrive/volume
    gemdrive ls [PATH]                            GET  /api/v1/gemdrive/files?path=...
    gemdrive get REMOTE [LOCAL] [-r/--resume]     GET  /api/v1/gemdrive/files/<rel>
    gemdrive put LOCAL [REMOTE] [-f/--force]      PUT  /api/v1/gemdrive/files/<rel>
    gemdrive rm REMOTE                            DEL  /api/v1/gemdrive/files/<rel>
    gemdrive mv FROM TO                           POST /api/v1/gemdrive/files/<from>/rename
    gemdrive mkdir REMOTE                         POST /api/v1/gemdrive/folders/<rel>
    gemdrive rmdir REMOTE                         DEL  /api/v1/gemdrive/folders/<rel>
    gemdrive mvdir FROM TO                        POST /api/v1/gemdrive/folders/<from>/rename
    runner …                                      /api/v1/runner/*    (see docs/api.md)
    debug …                                       /api/v1/debug, /api/v1/debug/log

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
    SIDECART_HOST=192.168.1.42 python3 cli/sidecart.py gemdrive ls /games
    python3 cli/sidecart.py gemdrive get FILE.TOS -r
    python3 cli/sidecart.py gemdrive put LOCAL.PRG -f
    python3 cli/sidecart.py gemdrive mvdir OLDNAME NEWNAME
    python3 cli/sidecart.py runner run /HELLODBG.TOS
    python3 cli/sidecart.py debug status
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
    """GET /api/v1/gemdrive/volume → print free / total / fs_type."""
    url = base_url(args.host) + "/api/v1/gemdrive/volume"
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
    """Build /api/v1/gemdrive/folders/<remote>, URL-encoding everything except '/'."""
    encoded = urllib.parse.quote(remote.lstrip("/"), safe="/")
    return base_url(host) + "/api/v1/gemdrive/folders/" + encoded


def _files_url(host: str, remote: str) -> str:
    """Build /api/v1/gemdrive/files/<remote>, URL-encoding everything except '/'."""
    encoded = urllib.parse.quote(remote.lstrip("/"), safe="/")
    return base_url(host) + "/api/v1/gemdrive/files/" + encoded


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
    """POST /api/v1/gemdrive/folders/<remote>."""
    url = _folders_url(args.host, args.remote)
    return _do_mutation(args, "POST", url, body=None, headers=None)


def cmd_rmdir(args: argparse.Namespace) -> int:
    """DELETE /api/v1/gemdrive/folders/<remote>."""
    url = _folders_url(args.host, args.remote)
    return _do_mutation(args, "DELETE", url, body=None, headers=None)


def cmd_mvdir(args: argparse.Namespace) -> int:
    """POST /api/v1/gemdrive/folders/<from>/rename body {'to': '<to>'}."""
    url = _folders_url(args.host, args.from_) + "/rename"
    body = json.dumps({"to": args.to}, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    return _do_mutation(args, "POST", url, body=body, headers=headers)


def cmd_rm(args: argparse.Namespace) -> int:
    """DELETE /api/v1/gemdrive/files/<remote>."""
    url = _files_url(args.host, args.remote)
    return _do_mutation(args, "DELETE", url, body=None, headers=None)


def cmd_mv(args: argparse.Namespace) -> int:
    """POST /api/v1/gemdrive/files/<from>/rename body {'to': '<to>'}."""
    url = _files_url(args.host, args.from_) + "/rename"
    body = json.dumps({"to": args.to}, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    return _do_mutation(args, "POST", url, body=body, headers=headers)


def _format_progress(done: int, total: int) -> str:
    if total > 0:
        return f"{done // 1024} KB / {total // 1024} KB"
    return f"{done // 1024} KB"


def cmd_get(args: argparse.Namespace) -> int:
    """GET /api/v1/gemdrive/files/<remote> → stream raw bytes to LOCAL."""
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


def cmd_runner_run(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/run — fire-and-forget Pexec."""
    cmdline = " ".join(args.cmdline) if args.cmdline else ""
    body_json = {"path": args.remote, "cmdline": cmdline}
    body = json.dumps(body_json, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/run"
    try:
        status, parsed, raw = request_json(
            "POST", url, body=body, headers=headers)
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
        print(f"ok  EXECUTE {args.remote} sent")
    return EXIT_OK


def cmd_runner_load(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/load — synchronous Pexec(3) load-only.

    Returns the basepage pointer on success. The program is then in
    m68k RAM, ready for `runner exec`. Use `runner status` to inspect
    the loaded state at any time.
    """
    cmdline = " ".join(args.cmdline) if args.cmdline else ""
    body_json = {"path": args.remote, "cmdline": cmdline}
    body = json.dumps(body_json, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/load"
    try:
        # Synchronous on the server side (spin-waits up to 10 s); use
        # a slightly longer client timeout so we never give up before
        # the device does.
        status, parsed, raw = request_json(
            "POST", url, body=body, headers=headers, timeout=15)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 200:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        if parsed is not None:
            json.dump(parsed, sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
    elif not args.quiet:
        bp = parsed.get("basepage", 0) if parsed else 0
        print(f"ok  LOAD {args.remote} → basepage 0x{bp:08X}")
    return EXIT_OK


def cmd_runner_exec(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/exec — fire-and-forget Pexec(4).

    Executes the program previously loaded via `runner load`. No
    arguments — the basepage pointer is held server-side as state.
    Re-exec on the same loaded program is supported (Pexec(4) does
    not free the basepage); use `runner unload` to release the
    memory when you're done.
    """
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/exec"
    try:
        status, parsed, raw = request_json(
            "POST", url, body=b"", headers=headers)
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
        print("ok  EXEC sent")
    return EXIT_OK


def cmd_runner_unload(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/unload — synchronous GEMDOS Mfree.

    Releases the basepage of the program previously loaded via
    `runner load`. Pairs with the Pexec(4)-based `runner exec`
    (which deliberately doesn't auto-free, so re-exec works).
    """
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/unload"
    try:
        # Server spin-waits up to 5 s; client allows a bit more.
        status, parsed, raw = request_json(
            "POST", url, body=b"", headers=headers, timeout=10)
    except urllib.error.URLError as exc:
        print(f"error: cannot reach {url}: {exc.reason}", file=sys.stderr)
        return EXIT_NETWORK

    if status != 200:
        render_error(parsed, raw, status)
        return status_to_exit_code(status)

    if args.json:
        if parsed is not None:
            json.dump(parsed, sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
    elif not args.quiet:
        bp = parsed.get("basepage", 0) if parsed else 0
        print(f"ok  UNLOAD basepage 0x{int(bp):08X}")
    return EXIT_OK


def _parse_adv_jump_address(raw: str) -> int:
    """Parse a jump-target address from CLI input.

    Accepts: decimal (e.g. "65536"), legacy hex with `$` prefix
    (e.g. "$FA1C00"), modern hex with `0x` prefix (e.g. "0xFA1C00").
    Raises ValueError on parse failure or out-of-range / odd values.

    Shell gotcha: bash/zsh expand `$78000` as a variable reference
    (unset variable → empty string → dropped from argv entirely),
    so `$hex` arguments MUST be single-quoted on the command line:
    `'$78000'`. Use the `0x78000` form to avoid quoting altogether.
    """
    s = raw.strip()
    if not s:
        raise ValueError("address is empty")
    if s.startswith("$"):
        value = int(s[1:], 16)
    elif s.startswith(("0x", "0X")):
        value = int(s, 16)
    else:
        value = int(s, 10)
    if value < 0 or value > 0xFFFFFF:
        raise ValueError(
            f"address 0x{value:X} out of 24-bit range ($0..$FFFFFF)")
    if value & 1:
        raise ValueError(
            f"address 0x{value:X} is odd; m68k 68000 instructions "
            f"must be even-aligned")
    return value


def cmd_runner_adv_jump(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/adv/jump — VBL ISR rte to user address."""
    try:
        addr = _parse_adv_jump_address(args.address)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_BAD_REQUEST

    body_json = {"address": f"0x{addr:X}"}
    body = json.dumps(body_json, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/adv/jump"
    try:
        status, parsed, raw = request_json(
            "POST", url, body=body, headers=headers)
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
        print(f"ok  ADV JUMP 0x{addr:06X} sent (VBL ISR rte)")
    return EXIT_OK


def cmd_runner_adv_load(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/adv/load — stream a workstation file into m68k RAM."""
    try:
        addr = _parse_adv_jump_address(args.address)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_BAD_REQUEST

    cap: int | None = None
    if args.size is not None:
        try:
            cap = _parse_adv_jump_address(args.size)
        except ValueError:
            # _parse_adv_jump_address enforces even+24bit which doesn't
            # apply to a byte count. Fall back to a plain int parse.
            s = args.size.strip()
            try:
                if s.startswith("$"):
                    cap = int(s[1:], 16)
                elif s.startswith(("0x", "0X")):
                    cap = int(s, 16)
                else:
                    cap = int(s, 10)
            except ValueError:
                print(f"error: cannot parse size: {args.size!r}",
                      file=sys.stderr)
                return EXIT_BAD_REQUEST
        if cap <= 0:
            print(f"error: size must be > 0", file=sys.stderr)
            return EXIT_BAD_REQUEST

    try:
        with open(args.local, "rb") as f:
            blob = f.read()
    except OSError as exc:
        print(f"error: cannot read {args.local}: {exc}", file=sys.stderr)
        return EXIT_BAD_REQUEST

    if cap is not None and cap < len(blob):
        blob = blob[:cap]

    if not blob:
        print(f"error: nothing to upload (empty file or size=0)",
              file=sys.stderr)
        return EXIT_BAD_REQUEST

    query = f"address=0x{addr:X}"
    if cap is not None:
        query += f"&size={cap}"
    url = base_url(args.host) + "/api/v1/runner/adv/load?" + query
    headers = {"Content-Type": "application/octet-stream"}
    try:
        status, parsed, raw = request_json(
            "POST", url, body=blob, headers=headers)
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
        bytes_sent = parsed.get("bytes", len(blob))
        print(f"ok  ADV LOAD {args.local} → 0x{addr:06X} "
              f"({bytes_sent} bytes)")
    return EXIT_OK


def cmd_runner_adv_meminfo(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/adv/meminfo — meminfo from inside the VBL ISR."""
    url = base_url(args.host) + "/api/v1/runner/adv/meminfo"
    try:
        status, parsed, raw = request_json("POST", url, body=b"")
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

    print(f"membottom [$432]  : 0x{parsed.get('membottom', 0):08X}")
    print(f"memtop    [$436]  : 0x{parsed.get('memtop', 0):08X}")
    print(f"phystop   [$42E]  : 0x{parsed.get('phystop', 0):08X}")
    print(f"screenmem [$44E]  : 0x{parsed.get('screenmem', 0):08X}")
    bp = parsed.get('basepage', 0)
    if bp:
        print(f"basepage  [$4F2]  : 0x{bp:08X}")
    else:
        print(f"basepage  [$4F2]  : 0 (TOS < 1.04 or unset)")
    b0 = parsed.get('bank0_kb', 0)
    b1 = parsed.get('bank1_kb', 0)
    if parsed.get('decoded'):
        print(f"bank 0    [$FF8001 nibble] : {b0} KB")
        print(f"bank 1    [$FF8001 nibble] : {b1} KB")
        print(f"total RAM         : {b0 + b1} KB")
    else:
        print(f"banks     [$FF8001 nibble] : (unrecognised MMU config)")
    return EXIT_OK


def cmd_runner_adv_status(args: argparse.Namespace) -> int:
    """GET /api/v1/runner/adv — Advanced Runner VBL hook state."""
    url = base_url(args.host) + "/api/v1/runner/adv"
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
    installed = parsed.get("installed", False)
    hook_vec = parsed.get("hook_vector", "unknown")
    print(f"runner active : {'yes' if active else 'no'}")
    if installed:
        if hook_vec == "vbl":
            print(f"hook vector   : installed (vbl @ $70)")
        elif hook_vec == "etv_timer":
            print(f"hook vector   : installed (etv_timer @ $400)")
        else:
            print(f"hook vector   : installed ({hook_vec})")
    else:
        print(f"hook vector   : not installed")
    return EXIT_OK


def cmd_debug_tail(args: argparse.Namespace) -> int:
    """GET /api/v1/debug/log — stream debug bytes until interrupted.

    Connects to the chunked-transfer-encoding endpoint and writes
    raw response bytes to stdout (the m68k's debug bytes show up
    on the workstation as if they came off a serial port). The
    connection stays open until the user kills it with Ctrl-C, the
    server closes (e.g., device reboot), or the network drops.
    """
    url = base_url(args.host) + "/api/v1/debug/log"
    req = urllib.request.Request(
        url, method="GET",
        headers={"User-Agent": USER_AGENT, "Accept": "application/octet-stream"})
    try:
        # No timeout — we deliberately want to block forever.
        resp = urllib.request.urlopen(req, timeout=None)
    except urllib.error.HTTPError as exc:
        # Non-200 from the server — render the error envelope if any.
        try:
            raw = exc.read() if exc.fp is not None else b""
        finally:
            exc.close()
        parsed: dict | None = None
        if raw:
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
        # Read in small chunks so the bytes appear on the workstation
        # as soon as the device sends them. urlopen + chunked is
        # streaming-friendly: each .read(N) returns whatever's
        # available without waiting for N bytes.
        out = sys.stdout.buffer
        while True:
            chunk = resp.read(256)
            if not chunk:
                break  # server closed
            out.write(chunk)
            out.flush()
    except KeyboardInterrupt:
        return EXIT_OK
    finally:
        resp.close()
    return EXIT_OK


def cmd_debug_status(args: argparse.Namespace) -> int:
    """GET /api/v1/debug — fast-debug-traces diagnostics (Epic 05 v2)."""
    url = base_url(args.host) + "/api/v1/debug"
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

    fw = parsed.get("firmware_mode", False)
    used = parsed.get("ring_used", 0)
    cap = parsed.get("ring_capacity", 0)
    dropped = parsed.get("bytes_dropped", 0)
    usbcdc_attached = parsed.get("usbcdc_attached", False)
    usbcdc_dropped = parsed.get("usbcdc_dropped", 0)
    print(f"firmware_mode  : {'yes' if fw else 'no'}")
    print(f"ring           : {used} / {cap} bytes")
    print(f"bytes_dropped  : {dropped}")
    print(f"usbcdc_attached: {'yes' if usbcdc_attached else 'no'}")
    print(f"usbcdc_dropped : {usbcdc_dropped}")
    return EXIT_OK


def cmd_runner_meminfo(args: argparse.Namespace) -> int:
    """GET /api/v1/runner/meminfo — synchronous system memory snapshot."""
    url = base_url(args.host) + "/api/v1/runner/meminfo"
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

    # ST sysvar addresses each field is read from (TOS docs).
    print(f"membottom [$432]  : 0x{parsed.get('membottom', 0):08X}")
    print(f"memtop    [$436]  : 0x{parsed.get('memtop', 0):08X}")
    print(f"phystop   [$42E]  : 0x{parsed.get('phystop', 0):08X}")
    print(f"screenmem [$44E]  : 0x{parsed.get('screenmem', 0):08X}")
    bp = parsed.get('basepage', 0)
    if bp:
        print(f"basepage  [$4F2]  : 0x{bp:08X}")
    else:
        print(f"basepage  [$4F2]  : 0 (TOS < 1.04 or unset)")
    b0 = parsed.get('bank0_kb', 0)
    b1 = parsed.get('bank1_kb', 0)
    if parsed.get('decoded'):
        print(f"bank 0    [$FF8001 nibble] : {b0} KB")
        print(f"bank 1    [$FF8001 nibble] : {b1} KB")
        print(f"total RAM         : {b0 + b1} KB")
    else:
        print(f"banks     [$FF8001 nibble] : (unrecognised MMU config)")
    return EXIT_OK


def cmd_runner_res(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/res — fire-and-forget XBIOS Setscreen."""
    body_json = {"rez": args.rez}
    body = json.dumps(body_json, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/res"
    try:
        status, parsed, raw = request_json(
            "POST", url, body=body, headers=headers)
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
        print(f"ok  RES {args.rez} sent")
    return EXIT_OK


def cmd_runner_cd(args: argparse.Namespace) -> int:
    """POST /api/v1/runner/cd — fire-and-forget GEMDOS Dsetpath."""
    body_json = {"path": args.remote}
    body = json.dumps(body_json, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    url = base_url(args.host) + "/api/v1/runner/cd"
    try:
        status, parsed, raw = request_json(
            "POST", url, body=body, headers=headers)
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
        print(f"ok  CD {args.remote} sent")
    return EXIT_OK


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
    last_cd_errno = parsed.get("last_cd_errno")
    print(f"active   : true")
    print(f"busy     : {'yes' if busy else 'no'}")
    print(f"cwd      : {cwd}")
    last_res_errno = parsed.get("last_res_errno")
    if last_cmd is None:
        print(f"last     : (no command run yet)")
    elif last_cmd == "CD":
        target = last_path if last_path is not None else "?"
        print(f"last     : CD {target} (errno={last_cd_errno})")
    elif last_cmd == "RES":
        print(f"last     : RES (errno={last_res_errno})")
    elif last_path is None:
        print(f"last     : {last_cmd} (exit={last_exit})")
    else:
        print(f"last     : {last_cmd} {last_path} (exit={last_exit})")

    # Epic 06 / S5+S6 — Pexec load+exec split state. Print only
    # when there's a basepage pending or a load error to report,
    # so a fresh-boot status stays uncluttered.
    loaded_basepage = parsed.get("loaded_basepage")
    last_load_errno = parsed.get("last_load_errno")
    if loaded_basepage is not None:
        print(f"loaded   : basepage 0x{int(loaded_basepage):08X}")
    elif last_load_errno is not None:
        print(f"loaded   : (last load failed, GEMDOS errno {last_load_errno})")
    return EXIT_OK


def cmd_put(args: argparse.Namespace) -> int:
    """PUT /api/v1/gemdrive/files/<remote>?overwrite=0|1 — stream LOCAL up.

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
    path = "/api/v1/gemdrive/files/" + encoded
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
    """GET /api/v1/gemdrive/files?path=PATH → print entries."""
    path = args.path or "/"
    encoded = urllib.parse.quote(path, safe="/")
    url = base_url(args.host) + f"/api/v1/gemdrive/files?path={encoded}"
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

    # Epic 06 / S10 — file/folder management verbs grouped under
    # `gemdrive` so they nest at the same depth as `runner` and
    # `debug` instead of polluting the top level. The HTTP API
    # (/api/v1/gemdrive/files/*, /api/v1/gemdrive/folders/*, /api/v1/gemdrive/volume) is
    # unchanged — purely a CLI-surface reorganization.
    gd = sub.add_parser(
        "gemdrive",
        help="Manage files / folders on the GEMDRIVE-emulated drive.")
    gd_sub = gd.add_subparsers(dest="gemdrive_cmd", required=True)
    gd_sub.add_parser("volume", help="Show SD card total / free space.")
    ls = gd_sub.add_parser("ls", help="List a folder on the SD card.")
    ls.add_argument("path", nargs="?", default="/",
                    help="Folder path (default: /).")
    mkdir = gd_sub.add_parser("mkdir", help="Create a folder.")
    mkdir.add_argument("remote", help="Folder path on the SD card.")
    rmdir = gd_sub.add_parser("rmdir", help="Delete an empty folder.")
    rmdir.add_argument("remote", help="Folder path on the SD card.")
    mvdir = gd_sub.add_parser("mvdir", help="Rename or move a folder.")
    mvdir.add_argument("from_", metavar="FROM",
                       help="Source folder path.")
    mvdir.add_argument("to", help="Destination folder path.")
    rm = gd_sub.add_parser("rm", help="Delete a file.")
    rm.add_argument("remote", help="File path on the SD card.")
    mv = gd_sub.add_parser("mv", help="Rename or move a file.")
    mv.add_argument("from_", metavar="FROM", help="Source file path.")
    mv.add_argument("to", help="Destination file path.")
    get = gd_sub.add_parser("get", help="Download a file.")
    get.add_argument("remote", help="File path on the SD card.")
    get.add_argument("local", nargs="?", default=None,
                     help="Local destination (default: basename of REMOTE).")
    get.add_argument("-r", "--resume", action="store_true",
                     help="Resume partial download via Range:.")
    put = gd_sub.add_parser("put", help="Upload a file.")
    put.add_argument("local", help="Local file to upload.")
    put.add_argument("remote", nargs="?", default=None,
                     help="Remote destination (default: basename of LOCAL).")
    put.add_argument("-f", "--force", action="store_true",
                     help="Overwrite if the remote file exists.")

    runner = sub.add_parser("runner", help="Runner mode (Epic 03).")
    runner_sub = runner.add_subparsers(dest="runner_cmd", required=True)
    runner_sub.add_parser("status", help="Show Runner state and last completion.")
    runner_sub.add_parser("reset", help="Cold-reset the Atari ST.")
    cd_p = runner_sub.add_parser(
        "cd", help="GEMDOS Dsetpath — change the Runner's cwd.")
    cd_p.add_argument(
        "remote", help="Directory (relative to GEMDRIVE_FOLDER).")
    res_p = runner_sub.add_parser(
        "res", help="XBIOS Setscreen — change ST screen rez (colour only).")
    res_p.add_argument(
        "rez", choices=["low", "med"],
        help="Target rez. Ignored on monochrome monitors.")
    runner_sub.add_parser(
        "meminfo",
        help="System memory snapshot from the live ST (synchronous).")
    adv_p = runner_sub.add_parser(
        "adv", help="Advanced Runner (Epic 04) — VBL hook diagnostics.")
    adv_sub = adv_p.add_subparsers(dest="adv_cmd", required=True)
    adv_sub.add_parser(
        "status",
        help="Show whether the Advanced Runner VBL hook is installed "
             "and which vector ($70 or $400) it landed on.")
    adv_sub.add_parser(
        "meminfo",
        help="System memory snapshot — same fields as `runner meminfo` "
             "but read from inside the VBL ISR, so it works against "
             "wedged programs the foreground meminfo can't reach.")
    jump_p = adv_sub.add_parser(
        "jump",
        help="Patch the VBL ISR's saved PC so the rte resumes at the "
             "given address. Fire-and-forget. Requires the VBL hook "
             "(\\$70). Address: decimal, $hex, or 0xhex; 24-bit; even. "
             "Shell gotcha: single-quote `$hex` ('$FA1C00') or your "
             "shell will expand it as a variable; or use 0xhex.")
    jump_p.add_argument(
        "address",
        help="Target address: decimal, $hex (legacy — single-quote "
             "in the shell: '$FA1C00'), or 0xhex (no quoting needed).")
    load_p = adv_sub.add_parser(
        "load",
        help="Stream a workstation file into m68k RAM through the VBL "
             "ISR. Synchronous — chunked through APP_FREE 8 KB at a "
             "time. Requires the VBL hook ($70). Target must be even, "
             "fit inside RAM (above 0x800, below phystop). Shell "
             "gotcha: single-quote `$hex` ('$78000') or your shell "
             "will expand it as a variable; or use 0xhex.")
    load_p.add_argument(
        "local",
        help="Workstation file to upload (raw bytes, no envelope).")
    load_p.add_argument(
        "address",
        help="Target start address: decimal, $hex (legacy — single-"
             "quote in the shell: '$78000'), or 0xhex. Even; 24-bit; "
             ">= 0x800.")
    load_p.add_argument(
        "size", nargs="?", default=None,
        help="Optional cap on bytes to upload (decimal, $hex, or "
             "0xhex; same shell-quoting rule as `address`). If smaller "
             "than the file, the trailing bytes are dropped "
             "(cap-and-truncate).")
    run_p = runner_sub.add_parser(
        "run", help="Run a .TOS / .PRG on the Atari ST.")
    run_p.add_argument("remote", help="Path to the program (relative to GEMDRIVE_FOLDER).")
    run_p.add_argument("cmdline", nargs=argparse.REMAINDER,
                       help="Command-line arguments (joined with spaces, ≤127 chars). "
                            "Everything after REMOTE is captured verbatim, including "
                            "leading dashes — no need for --.")
    load_p = runner_sub.add_parser(
        "load",
        help="Load a .TOS / .PRG into m68k RAM (Pexec mode 3) "
             "without executing it. Pair with `runner exec`.")
    load_p.add_argument("remote", help="Path to the program (relative to GEMDRIVE_FOLDER).")
    load_p.add_argument("cmdline", nargs=argparse.REMAINDER,
                        help="Command-line arguments stored in the basepage at load "
                             "time. Same quoting rule as `runner run`.")
    runner_sub.add_parser(
        "exec",
        help="Execute the program previously loaded via `runner load` "
             "(Pexec mode 4). No arguments — the basepage is held "
             "server-side as state. Re-exec is supported; use "
             "`runner unload` to free the memory.")
    runner_sub.add_parser(
        "unload",
        help="Free the basepage previously loaded via `runner load` "
             "(GEMDOS Mfree). Pairs with `runner exec` to give "
             "Pexec(0)-equivalent lifecycle without the auto-free.")

    debug = sub.add_parser(
        "debug",
        help="Fast-debug-traces (Epic 05) diagnostics + transports.")
    debug_sub = debug.add_subparsers(dest="debug_cmd", required=True)
    debug_sub.add_parser(
        "status",
        help="Show whether firmware mode has committed and the "
             "current debug-byte ring occupancy / drop count.")
    debug_sub.add_parser(
        "tail",
        help="Stream debug bytes as the device emits them. "
             "Like `tail -f` for the m68k debug output. Runs "
             "until Ctrl-C, server close, or network drop.")
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.host = resolve_host(args.host)

    handlers = {
        "ping": cmd_ping,
    }
    if args.cmd == "gemdrive":
        # Epic 06 / S10 — file/folder verbs grouped under
        # `gemdrive` so the CLI's nesting depth matches the
        # other subcommand families (runner, debug). HTTP API
        # endpoints are unchanged.
        gd_handlers = {
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
        handler = gd_handlers.get(args.gemdrive_cmd)
        if handler is None:
            parser.error(f"unknown gemdrive subcommand: {args.gemdrive_cmd}")
            return EXIT_USAGE
        return handler(args)
    if args.cmd == "runner":
        runner_handlers = {
            "status": cmd_runner_status,
            "reset": cmd_runner_reset,
            "run": cmd_runner_run,
            "load": cmd_runner_load,
            "exec": cmd_runner_exec,
            "unload": cmd_runner_unload,
            "cd": cmd_runner_cd,
            "res": cmd_runner_res,
            "meminfo": cmd_runner_meminfo,
        }
        if args.runner_cmd == "adv":
            adv_handlers = {
                "status": cmd_runner_adv_status,
                "meminfo": cmd_runner_adv_meminfo,
                "jump": cmd_runner_adv_jump,
                "load": cmd_runner_adv_load,
            }
            handler = adv_handlers.get(args.adv_cmd)
            if handler is None:
                parser.error(f"unknown runner adv subcommand: {args.adv_cmd}")
                return EXIT_USAGE
            return handler(args)
        handler = runner_handlers.get(args.runner_cmd)
        if handler is None:
            parser.error(f"unknown runner subcommand: {args.runner_cmd}")
            return EXIT_USAGE
        return handler(args)
    if args.cmd == "debug":
        debug_handlers = {
            "status": cmd_debug_status,
            "tail": cmd_debug_tail,
        }
        handler = debug_handlers.get(args.debug_cmd)
        if handler is None:
            parser.error(f"unknown debug subcommand: {args.debug_cmd}")
            return EXIT_USAGE
        return handler(args)
    handler = handlers.get(args.cmd)
    if handler is None:
        parser.error(f"unknown subcommand: {args.cmd}")
        return EXIT_USAGE
    return handler(args)


if __name__ == "__main__":
    sys.exit(main())
