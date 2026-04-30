#!/usr/bin/env python3
"""
sidecart — Python CLI for the md-devops Remote HTTP Management API.

Single-file, stdlib-only (Python >= 3.10). See docs/epics/02-http-api.md
for the full spec. Default host is sidecart.local (override with --host
or the SIDECART_HOST env var).

Usage:
    python3 cli/sidecart.py ping
    python3 cli/sidecart.py --host 192.168.1.42 ping
    SIDECART_HOST=192.168.1.42 python3 cli/sidecart.py ping --json

Subcommand coverage grows alongside the server (Epic 02 stories S1–S6).
S1 ships only `ping`.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
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
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.host = resolve_host(args.host)

    handlers = {
        "ping": cmd_ping,
    }
    handler = handlers.get(args.cmd)
    if handler is None:
        parser.error(f"unknown subcommand: {args.cmd}")
        return EXIT_USAGE
    return handler(args)


if __name__ == "__main__":
    sys.exit(main())
