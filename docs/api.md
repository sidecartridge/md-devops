# DevOps Microfirmware — Remote HTTP Management API

A small REST API the Pico W exposes on its Wi-Fi interface for
managing files on the SD card from a workstation. It's the
back-end the `cli/sidecart.py` CLI talks to; you can also drive it
directly from `curl`, `wget`, or any other HTTP client.

## Quick start

After flashing the firmware, open the device's setup menu — the
bottom of the screen prints the leased IP and the mDNS hostname,
e.g.

```
API : http://sidecart.local/  (192.168.1.50)
```

You can address the API at either form. Then:

```sh
# Probe.
curl http://sidecart.local/api/v1/ping
python3 cli/sidecart.py ping

# List the GEMDRIVE folder.
curl 'http://sidecart.local/api/v1/files?path=/'
python3 cli/sidecart.py ls /

# Upload a file (overwriting if it exists).
curl --upload-file ./SWITCHER.TOS \
     'http://sidecart.local/api/v1/files/SWITCHER.TOS?overwrite=1'
python3 cli/sidecart.py put SWITCHER.TOS -f

# Download a file (resume-aware).
curl -C - -o SWITCHER.TOS http://sidecart.local/api/v1/files/SWITCHER.TOS
python3 cli/sidecart.py get SWITCHER.TOS -r

# Delete a file.
curl -X DELETE http://sidecart.local/api/v1/files/SWITCHER.TOS
python3 cli/sidecart.py rm SWITCHER.TOS
```

## Conventions

- **No auth.** The API is open. Treat the network it's reachable on
  as trusted. Don't expose `sidecart.local` past your LAN router.
- **Jailed root.** All paths are relative to the GEMDRIVE folder
  (`GEMDRIVE_FOLDER` aconfig param, default `/devops`). `..` is
  rejected, NUL bytes are rejected, the API can never read or write
  outside that root.
- **FAT 8.3 names only.** Stem ≤ 8 chars, extension ≤ 3 chars,
  ASCII, FAT-illegal chars rejected (`*?/\:<>"|+,;=[]`, leading
  dot, leading/trailing space, multiple dots). Paths are case-folded
  to uppercase before any FatFs call; listings always return the
  uppercase form regardless of how the entry was stored.
- **HTTP/1.1 only**, `Connection: close` only.
- **`Host:` header is mandatory.** Missing → `400 bad_request`.
- **`Transfer-Encoding: chunked` on uploads is not supported** —
  `PUT` requires an explicit `Content-Length`. Missing → `411`.
- **One body-streaming request at a time.** A second concurrent
  download or upload returns `503 busy` with `Retry-After: 1`.
  Listings, ping, volume, and metadata mutations are not gated.
- **Response always carries** `Content-Type`, `Content-Length` (or
  chunked listing), `Connection: close`, `Server: md-devops/<v>`.
  No `Date:` header — the device has no real-time clock.
- **Successful JSON** uses `Content-Type: application/json`. Raw
  downloads use `application/octet-stream`.
- **Error envelope** for every non-2xx JSON response:

  ```json
  { "ok": false, "code": "<symbol>", "message": "<human-readable>" }
  ```

## Status codes

| Status | When |
| --- | --- |
| `200 OK` | Successful read / overwrite / no-op rename. |
| `201 Created` | New folder or new file via `PUT`. Carries `Location:`. |
| `204 No Content` | Successful delete. |
| `206 Partial Content` | `Range:` download. Carries `Content-Range:`. |
| `400 Bad Request` | `bad_request` / `bad_path` / `bad_query` / `name_too_long`. |
| `404 Not Found` | `not_found`; `is_directory` / `is_file` when path resolves to the other namespace. |
| `405 Method Not Allowed` | Always carries `Allow:`. |
| `409 Conflict` | Target exists, non-empty folder delete, root delete, file open elsewhere, read-only. |
| `411 Length Required` | `PUT` without `Content-Length`. |
| `413 Payload Too Large` | Upload body > 4 MB. |
| `415 Unsupported Media Type` | Wrong request `Content-Type` on a JSON route. |
| `416 Range Not Satisfiable` | Range outside file bounds. Carries `Content-Range: bytes */<size>`. |
| `422 Unprocessable Entity` | Malformed JSON body, missing required field, listing-on-file, rename-into-own-descendant. |
| `500 Internal Server Error` | FatFs disk error. |
| `503 Service Unavailable` | Body-stream lock held, SD not mounted, or Runner busy with another command. Always carries `Retry-After: 1`. |
| `504 Gateway Timeout` | Runner endpoint waited > 1 s for the m68k to reply (`gateway_timeout`). |

## Error code vocabulary

Clients can switch on `code` reliably. All defined symbols:

`bad_request`, `bad_path`, `bad_query`, `name_too_long`, `not_found`,
`is_directory`, `is_file`, `conflict`, `length_required`,
`payload_too_large`, `range_invalid`, `bad_json`, `unprocessable`,
`unsupported_media`, `method_not_allowed`, `busy`, `disk_error`,
`internal_error`, `runner_inactive`, `gateway_timeout`, `no_snapshot`.

The last three are Runner-specific (see *Runner mode* below):
`runner_inactive` means the user didn't pick `[U]` at boot,
`gateway_timeout` means the Atari ST didn't reply within 1 s, and
`no_snapshot` means the m68k handshake completed but no snapshot
was recorded (should not happen in practice).

---

## Endpoints

### `GET /api/v1/ping` — health check

Returns the firmware version and uptime in seconds.

**Success** (`200`):
```json
{ "ok": true, "version": "v0.0.1dev", "uptime_s": 123 }
```

**`curl`**:
```sh
curl http://sidecart.local/api/v1/ping
```

**`sidecart`**:
```sh
python3 cli/sidecart.py ping
```

`HEAD` is also accepted and returns the same headers with no body.

---

### `GET /api/v1/volume` — SD card capacity

**Success** (`200`):
```json
{ "ok": true, "total_b": 8589934592, "free_b": 1073741824, "fs_type": "FAT32" }
```

`fs_type` is one of `FAT12`, `FAT16`, `FAT32`, `EXFAT`.

**Errors:** `503 busy` if the SD card is not mounted.

**`curl`**:
```sh
curl http://sidecart.local/api/v1/volume
```

**`sidecart`**:
```sh
python3 cli/sidecart.py volume
```

---

### `GET /api/v1/files?path=<rel>` — list folder

`path` defaults to `/`. Trailing slash optional. Listing the root is
`?path=/`, `?path=`, or omitting `path=` entirely.

**Success** (`200`, `Transfer-Encoding: chunked`):
```json
{
  "ok": true,
  "path": "/games",
  "entries": [
    { "name": "INVADERS.PRG", "size": 12345, "is_dir": false,
      "mtime": "2026-04-30T12:34:56" },
    { "name": "DATA",         "size": 0,     "is_dir": true,
      "mtime": "2026-04-29T08:00:00" }
  ],
  "truncated": false
}
```

- `name` is the FAT 8.3 short name as stored on disk (uppercase).
- `size` is `0` for directories.
- `mtime` is `null` if FatFs returned a zero / invalid date.
- All entries are listed (including dotfiles).
- `truncated: true` when the 1000-entry hard cap is hit.

**Errors:** `400 bad_path` (malformed `path`), `404 not_found` (path
missing), `422 is_file` (path resolves to a file — use the
download form below).

**`curl`**:
```sh
curl 'http://sidecart.local/api/v1/files?path=/games'
```

**`sidecart`**:
```sh
python3 cli/sidecart.py ls /games
```

---

### `GET /api/v1/files/<rel>` — download file

Optional `Range:` header for resume / partial. Three forms:
`bytes=N-M`, `bytes=N-`, `bytes=-N` (last N bytes). Multi-range
(comma) → `416`.

**Success** (`200` full / `206` partial). Headers:
```
Content-Type: application/octet-stream
Content-Length: <N>
Accept-Ranges: bytes
Content-Range: bytes <start>-<end>/<total>     (only on 206)
```

Body: raw file bytes.

`HEAD` returns headers only.

**Errors:** `404 not_found`, `404 is_directory` (path is a directory
— use `?path=`), `416 range_invalid` (carries
`Content-Range: bytes */<size>`), `503 busy` (another body request
in flight).

**`curl`**:
```sh
curl -o SWITV310.TOS http://sidecart.local/api/v1/files/SWITV310.TOS
# Resume from a partial download:
curl -C - -o SWITV310.TOS http://sidecart.local/api/v1/files/SWITV310.TOS
```

**`sidecart`**:
```sh
python3 cli/sidecart.py get SWITV310.TOS
python3 cli/sidecart.py get SWITV310.TOS -r          # resume
python3 cli/sidecart.py get SWITV310.TOS local.tos   # custom local name
```

---

### `PUT /api/v1/files/<rel>?overwrite=0|1` — upload file

Body is the raw file bytes. `Content-Length` is required.
`Content-Length: 0` is legal (creates an empty file or truncates an
existing one when `overwrite=1`). `overwrite=` defaults to `0`; any
value other than `0` or `1` returns `400 bad_query`.

Body cap: **4 MB** (`HTTP_MAX_UPLOAD_BYTES`). Larger → `413`.

**Success:**
- `201 Created` on a new file (carries
  `Location: /api/v1/files/<rel>`).
- `200 OK` on overwrite.

```json
{ "ok": true, "path": "/SWITV310.TOS", "size": 57436 }
```

**Errors:** `400 bad_path` / `name_too_long` / `bad_query`,
`404 not_found` (parent folder missing), `409 conflict` (file
exists and `overwrite=0`), `409 is_directory` (target path is a
directory), `411 length_required`, `413 payload_too_large`,
`503 busy`.

If the client closes the connection before `Content-Length` bytes
arrive — or FatFs returns an error mid-write — the partially-written
file is deleted before the response is sent.

**`curl`**:
```sh
curl --upload-file ./SWITV310.TOS \
     'http://sidecart.local/api/v1/files/SWITV310.TOS?overwrite=1'
```

**`sidecart`**:
```sh
python3 cli/sidecart.py put SWITV310.TOS              # default REMOTE = basename
python3 cli/sidecart.py put SWITV310.TOS -f           # overwrite
python3 cli/sidecart.py put local.tos REMOTE.TOS -f
```

---

### `DELETE /api/v1/files/<rel>` — delete file

**Success** (`204 No Content`, no body).

**Errors:** `400 bad_path`, `404 not_found`, `404 is_directory`
(path is a folder — use `DELETE /api/v1/folders/<rel>`),
`409 conflict` (file is open elsewhere or marked read-only).

**`curl`**:
```sh
curl -X DELETE http://sidecart.local/api/v1/files/STALE.TXT
```

**`sidecart`**:
```sh
python3 cli/sidecart.py rm STALE.TXT
```

---

### `POST /api/v1/files/<rel>/rename` — rename / move file

`Content-Type: application/json`. Body: `{"to":"<rel>"}`.

**Success** (`200`):
```json
{ "ok": true, "from": "/OLD.TXT", "to": "/NEW.TXT" }
```

`from == to` is a legal no-op and also returns `200`.

**Errors:** `400 bad_path` / `name_too_long`, `404 not_found`
(source missing), `404 not_found` (target's parent missing),
`404 is_directory` (source is a directory — use the folder route),
`409 conflict` (target exists), `411 length_required`,
`415 unsupported_media`, `422 bad_json` / `unprocessable`.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"to":"/NEW.TXT"}' \
     http://sidecart.local/api/v1/files/OLD.TXT/rename
```

**`sidecart`**:
```sh
python3 cli/sidecart.py mv OLD.TXT NEW.TXT
```

---

### `POST /api/v1/folders/<rel>` — create folder

**Success** (`201 Created`, carries `Location: /api/v1/folders/<rel>`):
```json
{ "ok": true, "path": "/SUB" }
```

**Errors:** `400 bad_path` / `name_too_long`, `404 not_found`
(parent folder missing), `409 conflict` (folder already exists or
root).

**`curl`**:
```sh
curl -X POST http://sidecart.local/api/v1/folders/SUB
```

**`sidecart`**:
```sh
python3 cli/sidecart.py mkdir SUB
```

---

### `DELETE /api/v1/folders/<rel>` — delete folder

**Success** (`204 No Content`).

**Errors:** `400 bad_path`, `404 not_found`, `404 is_file` (path is
a regular file — use `DELETE /api/v1/files/<rel>`),
`409 conflict` (folder is non-empty, locked open, read-only, or is
root).

**`curl`**:
```sh
curl -X DELETE http://sidecart.local/api/v1/folders/SUB
```

**`sidecart`**:
```sh
python3 cli/sidecart.py rmdir SUB
```

---

### `POST /api/v1/folders/<rel>/rename` — rename / move folder

Mirrors the file rename. Body: `{"to":"<rel>"}`. Cycle detection:
renaming a folder into its own descendant returns `422 unprocessable`.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"to":"/NEWNAME"}' \
     http://sidecart.local/api/v1/folders/OLDNAME/rename
```

**`sidecart`**:
```sh
python3 cli/sidecart.py mvdir OLDNAME NEWNAME
```

---

## Runner mode

Runner mode is a foreground execution loop that runs on the Atari
ST instead of the GEMDRIVE-only firmware. The user picks `[U]` at
the setup terminal to launch it; the m68k Runner stays in a poll
loop reading commands from the cartridge sentinel, while GEMDRIVE
keeps servicing TOS file I/O so programs you launch can use the
emulated drive normally.

All Runner endpoints live under `/api/v1/runner/`. Most are fire-
and-forget (`202 Accepted` — the m68k reports completion later via
the cartridge protocol, surfaced via `runner status`). `meminfo` is
synchronous and returns the live snapshot. Every Runner endpoint
also returns `409 runner_inactive` when the user did not pick `[U]`
at boot, and `409 busy` (with `Retry-After: 1`) when another Runner
command is already in flight.

---

### `GET /api/v1/runner` — Runner state

Reports whether Runner mode is active, whether a command is in
flight, the last-known cwd, and the most-recent completion's
metadata.

**`curl`**:
```sh
curl http://sidecart.local/api/v1/runner
```

**Response (200 OK)**:
```json
{
  "ok": true,
  "active": true,
  "busy": false,
  "cwd": "/GAMES/ARKANOID",
  "last_command": "EXECUTE",
  "last_path": "/GAMES/ARKANOID/RUNME.TOS",
  "last_exit_code": 0,
  "last_cd_errno": null,
  "last_res_errno": null,
  "last_started_at_ms": 12345,
  "last_finished_at_ms": 13002
}
```

`last_command` is one of `null`, `RESET`, `EXECUTE`, `CD`, `RES`,
or `MEMINFO`. The `last_cd_errno` / `last_res_errno` fields are
`null` unless the most-recent command was a CD or RES respectively.

**`sidecart`**:
```sh
python3 cli/sidecart.py runner status        # human form
python3 cli/sidecart.py runner status --json # raw envelope
```

---

### `POST /api/v1/runner/reset` — cold-reset the ST

Fires `RUNNER_CMD_RESET` at the m68k, which invalidates TOS' memory
cookies and jumps through `$4.w`. The Runner re-launches itself
automatically once the m68k cold-boot reaches `gemdrive_init`'s
HELLO handshake — no operator action needed. Stale state on the
RP side (busy lock, cwd mirror, last-cd / last-res errno) is
cleared by the m68k's HELLO message at re-entry.

**`curl`**:
```sh
curl -X POST http://sidecart.local/api/v1/runner/reset
```

**Response (202 Accepted)**: `{"ok":true,"accepted":true}`.

**`sidecart`**:
```sh
python3 cli/sidecart.py runner reset
```

---

### `POST /api/v1/runner/run` — Pexec a TOS / PRG program

Body: `{"path":"<rel>", "cmdline":"<≤127 chars>"}`. `cmdline` may be
omitted (defaults to empty). The path is jailed under
`GEMDRIVE_FOLDER` like every other API endpoint; relative paths
resolve against the current `runner cd` cwd, so after `cd /GAMES`
you can `run RUNME.TOS` without retyping the prefix.

The handler validates the path exists and is a regular file before
firing the sentinel, so 404 / 400 errors come back synchronously;
the `202` only means the Pexec was successfully dispatched. The
program's exit code shows up later in `runner status`.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"path":"/GAMES/ARKANOID/RUNME.TOS","cmdline":""}' \
     http://sidecart.local/api/v1/runner/run
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner run /GAMES/ARKANOID/RUNME.TOS
python3 cli/sidecart.py runner run RUNME.TOS                    # if cwd is /GAMES/ARKANOID
python3 cli/sidecart.py runner run PROG.TOS -- -v --file foo    # cmdline = "-v --file foo"
```

Common error codes: `404 not_found` (program file doesn't exist),
`400 bad_path` (rejected name), `400 bad_request` (cmdline too
long), `409 runner_inactive`, `409 busy`.

---

### `POST /api/v1/runner/cd` — change the Runner cwd

Body: `{"path":"<rel>"}`. The m68k issues a GEMDOS `Dsetpath` and
reports the GEMDOS errno via the cartridge protocol; the RP-side
mirror updates so subsequent relative `runner run` / `runner cd`
calls resolve from the new cwd. Allows `/` (Dsetpath to drive
root); validates that other paths exist and are directories before
firing.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"path":"/GAMES/ARKANOID"}' \
     http://sidecart.local/api/v1/runner/cd
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner cd /GAMES/ARKANOID
python3 cli/sidecart.py runner cd ARKANOID         # relative to current cwd
```

Common error codes: `404 not_found`, `400 bad_path` (rejected name
or "not a directory"), `409 runner_inactive`, `409 busy`.

---

### `POST /api/v1/runner/res` — change screen resolution

Body: `{"rez":"low"|"med"}`. Stateless — the caller passes the
target rez explicitly. The m68k inspects the current resolution
and refuses on monochrome (high-rez) monitors, reporting an errno
that surfaces as `last_res_errno` in `runner status` (`-1` =
ignored on mono, `-2` = bad rez). On colour monitors, the handler
calls XBIOS `Setscreen` followed by `Vsync`, restores the default
TOS palette, and repaints the runner banner.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"rez":"med"}' \
     http://sidecart.local/api/v1/runner/res
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner res low
python3 cli/sidecart.py runner res med
```

Common error codes: `400 bad_request` (unknown rez), `409
runner_inactive`, `409 busy`.

---

### `GET /api/v1/runner/meminfo` — system memory snapshot

Synchronous read of the ST's TOS memory cookies and the MMU
bank-config register. The handler fires `RUNNER_CMD_MEMINFO` and
spins on the chandler loop until the m68k replies, with a 1-second
timeout (returned as `504 gateway_timeout` if the m68k is wedged).

Reported addresses:

| Field       | ST sysvar    | Meaning |
| ----------- | ------------ | ------- |
| `membottom` | `$432`       | `_membot` — start of usable RAM after TOS reservations. |
| `memtop`    | `$436`       | `_memtop` — top of TPA memory available to processes. |
| `phystop`   | `$42E`       | `_phystop` — top of physical RAM. |
| `screenmem` | `$44E`       | `_v_bas_ad` — logical screen base. |
| `basepage`  | `$4F2`       | `_run` — current process basepage (TOS ≥ 1.04; 0 on older TOS). |
| `bank0_kb`, `bank1_kb` | `$FFFF8001` lower nibble | MMU bank sizes in KB. `0/0` when the nibble is unrecognised. |
| `decoded`   | derived | `true` when at least one bank size is non-zero. |

Bank decode table (matches Atari ST hardware):

| Nibble (bin) | bank0 | bank1 |
| ------------ | ----- | ----- |
| `0000`       | 128   | 128   |
| `0100`       | 512   | 128   |
| `0101`       | 512   | 512   |
| `1000`       | 2048  | 128   |
| `1010`       | 2048  | 2048  |

**`curl`**:
```sh
curl http://sidecart.local/api/v1/runner/meminfo
```

**Response (200 OK)**:
```json
{
  "ok": true,
  "membottom": 37544,
  "memtop": 3670016,
  "phystop": 4194304,
  "screenmem": 4161664,
  "basepage": 0,
  "bank0_kb": 2048,
  "bank1_kb": 2048,
  "decoded": true
}
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner meminfo
```

Common error codes: `409 runner_inactive`, `409 busy`,
`504 gateway_timeout`.

---

## CLI exit codes

`cli/sidecart.py` maps response status to a granular exit code so
shell scripts can branch:

| Code | Meaning |
| --- | --- |
| `0` | Success. |
| `1` | Generic / unexpected. |
| `2` | argparse usage error. |
| `3` | `404`. |
| `4` | `409`. |
| `5` | `400` or `422` (or any other 4xx that isn't `404` / `409` / `503`). |
| `6` | `503` (busy or SD not mounted). |
| `7` | `5xx` other than `503`. |
| `8` | Network / DNS error (couldn't reach the host). |

Override the host with `--host HOST[:PORT]` or env var
`SIDECART_HOST`. Default is `sidecart.local`. Add `--json` to any
subcommand to passthrough the raw JSON envelope; `-q`/`--quiet`
silences normal output (errors still go to stderr).
