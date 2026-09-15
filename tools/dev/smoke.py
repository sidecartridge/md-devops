#!/usr/bin/env python3
"""Hardware smoke script for md-devops (EPIC-10 STORY-01, reused by EPIC-16).

Runs what the HTTP API can reach, drives the setup menu through the Debug Probe
(swd.py) and prompts only for what needs a person at the ST. Between steps it
reads GET /api/v1/system/health and fails the run if the RP rebooted, counted a
crash or overran the ROM3 ring.

Usage:
    python3 tools/dev/smoke.py [--host 192.168.1.50] [--label release]
                               [--manual] [--entries 100] [--no-swd]
                               [--hellodbg PATH] [--json OUT]

--manual   ask for the steps only a person can do: the cold boot, the [G]
           desktop and file copy, and switching the ST to [U]. Without it those
           steps are skipped and reported as such.
--no-swd   do not use the Debug Probe. Stopping the countdown and pressing
           SELECT then become manual steps too (or skipped without --manual).

With the probe, the script stops the countdown, presses SELECT and reads the
menu text and the firmware's own state (swd.py app/select/text), so those steps
need nobody at the ST. A debug build is needed for the mailbox commands; on a
release build they fall back to prompts.

Limits that follow known bugs, raise them when those epics land:
    uploads stay at 256 KB  (EPIC-13 STORY-02: the idle sweeper closes longer uploads)
    no folder creation      (EPIC-11 STORY-03: mkdir panics on big-cluster cards)
    downloads stay at 256 KB (uploads are how the test file gets onto the card)
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import console  # noqa: E402  (same folder)
import swd  # noqa: E402

UPLOAD_BYTES = 256 * 1024


class Probe:
    """The Debug Probe side: menu control and firmware state over SWD."""

    def __init__(self, enabled: bool) -> None:
        self.elf: str | None = None
        self.error = "disabled" if not enabled else ""
        if not enabled:
            return
        try:
            self.elf = swd.matching_elf(None)
        except swd.SwdError as exc:
            self.error = str(exc)

    @property
    def ready(self) -> bool:
        return self.elf is not None

    @property
    def has_mailbox(self) -> bool:
        """The debug mailbox is in debug builds only."""
        if not self.ready:
            return False
        try:
            return bool(swd.elf_symbols(self.elf, swd.MAILBOX_SYMBOL))
        except swd.SwdError:
            return False

    def app(self, name: str) -> bool:
        """Run an app command from the debug mailbox."""
        command_id = swd.include_defines().get("DEVHOOKS_APP_" + name.upper())
        if command_id is None:
            raise swd.SwdError(f"no DEVHOOKS_APP_{name.upper()} define")
        return bool(swd.mailbox_request(self.elf, swd.KIND_APP, command_id, []))

    def select_short(self) -> None:
        defs = swd.include_defines()
        ctrl = swd.IO_BANK0 + 4 + 8 * defs["SELECT_GPIO"]
        normal = swd.read_word(ctrl) & ~(3 << swd.INOVER_SHIFT)
        pressed = normal | (swd.INOVER_HIGH << swd.INOVER_SHIFT)
        swd.openocd(f"mww 0x{ctrl:08x} 0x{pressed:08x}",
                    f"sleep {swd.SELECT_SHORT_MS}",
                    f"mww 0x{ctrl:08x} 0x{normal:08x}")

    def flag(self, name: str) -> bool | None:
        """A boolean variable of the firmware, None when the ELF lacks it."""
        sym = swd.elf_symbols(self.elf, name).get(name)
        if not sym:
            return None
        return swd.read_memory(sym[0], sym[1] or 1)[0] != 0

    def wait_for_text(self, needle: str, timeout: float = 60) -> bool:
        """Wait for the menu (or any text) to appear on the RP's screen. After
        a reset the boot screen is up for about 11 s before the menu."""
        deadline = time.time() + timeout
        while True:
            if needle in self.text():
                return True
            if time.time() >= deadline:
                return False
            time.sleep(1)

    def text(self) -> str:
        sym = swd.elf_symbols(self.elf, "screen").get("screen")
        if not sym or not sym[1]:
            return ""
        width = swd.include_defines().get("TERM_SCREEN_SIZE_X", 40)
        data = swd.read_memory(sym[0], sym[1])
        return "\n".join(
            "".join(chr(c) if 32 <= c < 127 else " " for c in
                    data[y * width:(y + 1) * width]).rstrip()
            for y in range(sym[1] // width))


class Smoke:
    def __init__(self, host: str, manual: bool, probe: "Probe") -> None:
        self.base = f"http://{host}/api/v1"
        self.host = host
        self.manual = manual
        self.probe = probe
        self.results: list[dict] = []
        self.start_health: dict | None = None
        self.last_health: dict | None = None

    # --- HTTP helpers -------------------------------------------------------

    def request(self, method: str, path: str, body: bytes | None = None,
                ctype: str | None = None, timeout: float = 20) -> tuple[int, bytes]:
        req = urllib.request.Request(self.base + path, data=body, method=method)
        if ctype:
            req.add_header("Content-Type", ctype)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()

    def json(self, method: str, path: str, body: dict | None = None,
             timeout: float = 20) -> tuple[int, dict]:
        data = json.dumps(body).encode() if body is not None else None
        status, raw = self.request(method, path, data,
                                   "application/json" if body is not None else None,
                                   timeout)
        try:
            return status, json.loads(raw)
        except ValueError:
            return status, {"raw": raw[:200].decode(errors="replace")}

    def health(self) -> dict:
        status, doc = self.json("GET", "/system/health", timeout=5)
        if status != 200:
            raise RuntimeError(f"health returned {status}")
        return doc

    # --- Result bookkeeping -------------------------------------------------

    def record(self, name: str, status: str, detail: str) -> None:
        self.results.append({"step": name, "status": status, "detail": detail})
        mark = {"pass": "PASS", "fail": "FAIL", "skip": "SKIP"}[status]
        print(f"[{mark}] {name}: {detail}", flush=True)

    def check_health(self, name: str) -> None:
        """Fail the step if the RP rebooted, crashed or overran since the last check."""
        prev = self.last_health
        try:
            now = self.health()
        except Exception as exc:  # noqa: BLE001 - report any failure to read health
            self.record(f"{name} / health", "fail", f"health unreadable: {exc}")
            return
        problems = []
        if prev is not None:
            if now["uptime_s"] < prev["uptime_s"]:
                problems.append(f"rebooted (uptime {prev['uptime_s']} -> {now['uptime_s']}, "
                                f"reset {now['reset']['reason']})")
            if now["reset"]["crash_count"] > prev["reset"]["crash_count"]:
                problems.append(f"crash count {prev['reset']['crash_count']} -> "
                                f"{now['reset']['crash_count']}")
            if now["rom3_overruns"] > prev["rom3_overruns"]:
                problems.append(f"ROM3 overruns {prev['rom3_overruns']} -> {now['rom3_overruns']}")
        detail = (f"up {now['uptime_s']} s, heap min {now['heap']['min_free']}, "
                  f"stack {now['stack']['high_water']}")
        self.record(f"{name} / health", "fail" if problems else "pass",
                    "; ".join(problems) if problems else detail)
        self.last_health = now

    def ask(self, prompt: str) -> bool | None:
        """Ask a yes/no question; None when not running with --manual."""
        if not self.manual:
            return None
        while True:
            answer = input(f"  {prompt} [y/n] ").strip().lower()
            if answer in ("y", "n"):
                return answer == "y"

    # --- Steps --------------------------------------------------------------

    def step_baseline(self) -> None:
        deadline = time.time() + 90
        while True:
            try:
                h = self.health()
                break
            except (OSError, urllib.error.URLError, RuntimeError) as exc:
                if time.time() > deadline:
                    self.record("baseline", "fail", f"device not reachable: {exc}")
                    raise SystemExit(1)
                time.sleep(2)
        self.start_health = self.last_health = h
        ok = h["watchdog"]
        build = h.get("build", "?")
        kind = "debug" if h.get("debug") else "release"
        self.record("baseline", "pass" if ok else "fail",
                    f"{h['version']} {build} ({kind}), heap total "
                    f"{h['heap']['total']}, reset {h['reset']['reason']}, "
                    f"crashes {h['reset']['crash_count']}, "
                    f"watchdog {'on' if ok else 'OFF'}")
        if self.probe.ready:
            self.record("baseline / probe", "pass",
                        f"{os.path.basename(self.probe.elf)}")
        else:
            self.record("baseline / probe", "skip", self.probe.error)

    def step_cold_boot(self) -> None:
        name = "1 cold boot to the menu"
        ok = self.ask("Power-cycle everything. Did the ST show the DevOps menu with the countdown running?")
        if ok is None:
            self.record(name, "skip", "needs --manual")
            return
        self.record(name, "pass" if ok else "fail", "reported by the tester")
        self.last_health = self.health()

    def step_stop_countdown(self) -> None:
        """Stop the boot countdown so the ST stays on the menu."""
        name = "1b stop the countdown"
        if self.probe.has_mailbox:
            try:
                # The menu comes up about 11 s after a reset; the countdown
                # only starts with it.
                menu = self.probe.wait_for_text("Select an option")
                done = self.probe.app("countdown_stop")
                halted = self.probe.flag("haltCountdown")
                passed = done and halted is not False and menu
                self.record(name, "pass" if passed else "fail",
                            f"mailbox {'ok' if done else 'refused'}, "
                            f"haltCountdown {halted}, menu text "
                            f"{'found' if menu else 'NOT found in 60 s'}")
                return
            except swd.SwdError as exc:
                self.record(name, "fail", f"probe: {exc}")
                return
        # A release build has no mailbox: ask instead.
        ok = self.ask("Stop the countdown. Is the ST on the menu?")
        if ok is None:
            self.record(name, "skip", "no mailbox (release build); needs --manual")
            return
        detail = "reported by the tester"
        if ok and self.probe.ready:
            menu = self.probe.wait_for_text("Select an option", 10)
            ok = ok and menu
            detail += f", menu text {'found' if menu else 'NOT found'}"
        self.record(name, "pass" if ok else "fail", detail)

    def step_gemdrive_desktop(self) -> None:
        name = "2 [G] desktop and file copy"
        ok = self.ask("Press [G]. Did TOS reach the desktop with the GEMDRIVE drive, "
                      "open a folder window and copy a file onto the drive?")
        if ok is None:
            self.record(name, "skip", "needs --manual")
            return
        self.record(name, "pass" if ok else "fail", "reported by the tester")
        self.check_health(name)

    def step_http_files(self) -> None:
        name = "4 HTTP upload, download, rename, delete"
        payload = os.urandom(UPLOAD_BYTES)
        status, _ = self.request("PUT", "/gemdrive/files/SMOKE.BIN?overwrite=1",
                                 payload, "application/octet-stream", timeout=60)
        if status not in (200, 201):
            self.record(name, "fail", f"upload returned {status}")
            return
        status, back = self.request("GET", "/gemdrive/files/SMOKE.BIN", timeout=60)
        if status != 200 or back != payload:
            self.record(name, "fail", f"download returned {status}, "
                        f"{'match' if back == payload else 'MISMATCH'}")
            return
        status, doc = self.json("POST", "/gemdrive/files/SMOKE.BIN/rename", {"to": "/SMOKE2.BIN"})
        if status != 200:
            self.record(name, "fail", f"rename returned {status}: {doc}")
            return
        status, _ = self.request("DELETE", "/gemdrive/files/SMOKE2.BIN")
        if status not in (200, 204):
            self.record(name, "fail", f"delete returned {status}")
            return
        status, _ = self.request("GET", "/gemdrive/files/SMOKE2.BIN")
        self.record(name, "pass" if status == 404 else "fail",
                    f"256 KB round trip matched; after delete GET returned {status}")
        self.check_health(name)

    def step_http_listing(self, entries: int) -> None:
        name = f"4 HTTP listing of {entries} entries"
        names = [f"SMK{i:05d}.TXT" for i in range(entries)]
        try:
            for n in names:
                status, _ = self.request("PUT", f"/gemdrive/files/{n}?overwrite=1",
                                         n.encode(), "application/octet-stream")
                if status not in (200, 201):
                    self.record(name, "fail", f"creating {n} returned {status}")
                    return
            t0 = time.time()
            status, doc = self.json("GET", "/gemdrive/files?path=/", timeout=60)
            elapsed = time.time() - t0
            listed = {e["name"] for e in doc.get("entries", [])}
            missing = [n for n in names if n not in listed]
            ok = status == 200 and not missing and not doc.get("truncated")
            self.record(name, "pass" if ok else "fail",
                        f"listing {status} in {elapsed:.1f} s, {len(listed)} entries, "
                        f"{len(missing)} missing, truncated {doc.get('truncated')}")
        finally:
            for n in names:
                self.request("DELETE", f"/gemdrive/files/{n}")
        self.check_health(name)

    def step_runner(self, hellodbg: str) -> None:
        name = "3+5 Runner load, exec, unload with a debug tail"
        _, st = self.json("GET", "/runner")
        if not st.get("active"):
            ok = self.ask("Runner is not active. Press [U] on the ST (or let the countdown launch it), "
                          "wait for [READY], then answer y")
            if not ok:
                self.record(name, "skip", "Runner not active")
                return
            _, st = self.json("GET", "/runner")
            if not st.get("active"):
                self.record(name, "fail", "Runner still not active")
                return
            # Getting to [U] may have taken a SELECT or ST reset: compare the
            # next health check with the device as it is now.
            self.last_health = self.health()
        try:
            with open(hellodbg, "rb") as f:
                program = f.read()
        except OSError as exc:
            self.record(name, "fail", f"test program unreadable: {exc}")
            return
        status, _ = self.request("PUT", "/gemdrive/files/SMOKEDBG.TOS?overwrite=1",
                                 program, "application/octet-stream")
        if status not in (200, 201):
            self.record(name, "fail", f"uploading the test program returned {status}")
            return

        tail_bytes = [0]
        stop = threading.Event()

        def tail() -> None:
            try:
                with urllib.request.urlopen(self.base + "/debug/log", timeout=30) as resp:
                    while not stop.is_set():
                        chunk = resp.read1(4096)
                        if not chunk:
                            break
                        tail_bytes[0] += len(chunk)
            except (OSError, urllib.error.URLError, socket.timeout):
                pass

        thread = threading.Thread(target=tail, daemon=True)
        thread.start()
        time.sleep(1.0)
        try:
            status, doc = self.json("POST", "/runner/load",
                                    {"path": "/SMOKEDBG.TOS", "cmdline": ""}, timeout=15)
            if status != 200:
                self.record(name, "fail", f"load returned {status}: {doc}")
                return
            basepage = doc.get("basepage")
            status, doc = self.json("POST", "/runner/exec", {}, timeout=15)
            if status != 202:
                self.record(name, "fail", f"exec returned {status}: {doc}")
                return
            deadline = time.time() + 15
            polls = 0
            exit_code = None
            while time.time() < deadline:
                time.sleep(0.5)
                polls += 1
                _, st = self.json("GET", "/runner")
                if not st.get("busy") and st.get("last_exit_code") is not None:
                    exit_code = st["last_exit_code"]
                    break
            status, doc = self.json("POST", "/runner/unload", {}, timeout=15)
            unload_ok = status == 200
            time.sleep(2.0)
        finally:
            stop.set()
            self.request("DELETE", "/gemdrive/files/SMOKEDBG.TOS")
        ok = exit_code == 0 and unload_ok and tail_bytes[0] > 0
        self.record(name, "pass" if ok else "fail",
                    f"basepage {basepage}, exit code {exit_code} after {polls} status polls, "
                    f"unload {'ok' if unload_ok else status}, debug tail {tail_bytes[0]} bytes")
        self.check_health(name)

    def step_select(self) -> None:
        name = "6 SELECT short press"
        before = self.health()
        if self.probe.ready:
            try:
                self.probe.select_short()
                ok = True
            except swd.SwdError as exc:
                self.record(name, "fail", f"probe: {exc}")
                return
            deadline = time.time() + 60
            while time.time() < deadline:
                try:
                    if self.health()["uptime_s"] < before["uptime_s"]:
                        break
                except (OSError, urllib.error.URLError, RuntimeError):
                    pass
                time.sleep(2)
        else:
            ok = self.ask("Tap SELECT once. Did the ST menu come back?")
        if ok is None:
            self.record(name, "skip", "needs --manual or the probe")
            return
        try:
            after = self.health()
        except Exception as exc:  # noqa: BLE001
            self.record(name, "fail", f"health unreadable after SELECT: {exc}")
            return
        rebooted = after["uptime_s"] < before["uptime_s"]
        reason = after["reset"]["reason"]
        menu = True
        if self.probe.ready:
            try:
                menu = self.probe.wait_for_text("Select an option")
            except swd.SwdError:
                menu = False
        passed = ok and rebooted and reason == "reset" and menu
        self.record(name, "pass" if passed else "fail",
                    f"menu {'back' if menu else 'NOT back'}, rebooted "
                    f"{rebooted}, reset reason {reason}")
        self.last_health = after

    def summary(self) -> int:
        fails = [r for r in self.results if r["status"] == "fail"]
        skips = [r for r in self.results if r["status"] == "skip"]
        passes = [r for r in self.results if r["status"] == "pass"]
        print(f"\n{len(passes)} passed, {len(fails)} failed, {len(skips)} skipped")
        return 1 if fails else 0


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default=os.environ.get("SIDECART_HOST", "sidecart.local"))
    parser.add_argument("--label", default="", help="build under test, for the report")
    parser.add_argument("--manual", action="store_true", help="ask for the ST-side steps")
    parser.add_argument("--no-swd", action="store_true",
                        help="do not use the Debug Probe")
    parser.add_argument("--entries", type=int, default=100, help="files for the listing step")
    parser.add_argument("--hellodbg", default=os.path.join(
        repo, "target", "atarist", "test", "hello-debug", "dist", "HELLODBG.TOS"))
    parser.add_argument("--json", help="write the results to this file")
    args = parser.parse_args()

    probe = Probe(not args.no_swd)
    smoke = Smoke(args.host, args.manual, probe)
    print(f"md-devops smoke {args.label} against {args.host} at {time.strftime('%Y-%m-%d %H:%M:%S')}")
    smoke.step_baseline()
    smoke.step_cold_boot()
    smoke.step_stop_countdown()
    smoke.step_gemdrive_desktop()
    smoke.step_http_files()
    smoke.step_http_listing(args.entries)
    smoke.step_runner(args.hellodbg)
    smoke.step_select()
    code = smoke.summary()
    if args.json:
        health = smoke.last_health or {}
        log = console.lines_since_boot(
            console.read_lines(console.LOG_DEFAULT), 1) or []
        with open(args.json, "w") as f:
            json.dump({"label": args.label, "host": args.host,
                       "build": health.get("build"),
                       "debug": health.get("debug"),
                       "elf": os.path.basename(probe.elf) if probe.ready else None,
                       "results": smoke.results,
                       "start_health": smoke.start_health,
                       "end_health": smoke.last_health,
                       "console_since_boot": log}, f, indent=2)
    return code


if __name__ == "__main__":
    sys.exit(main())
