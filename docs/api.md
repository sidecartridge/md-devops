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
curl 'http://sidecart.local/api/v1/gemdrive/files?path=/'
python3 cli/sidecart.py gemdrive ls /

# Upload a file (overwriting if it exists).
curl --upload-file ./SWITCHER.TOS \
     'http://sidecart.local/api/v1/gemdrive/files/SWITCHER.TOS?overwrite=1'
python3 cli/sidecart.py gemdrive put SWITCHER.TOS -f

# Download a file (resume-aware).
curl -C - -o SWITCHER.TOS http://sidecart.local/api/v1/gemdrive/files/SWITCHER.TOS
python3 cli/sidecart.py gemdrive get SWITCHER.TOS -r

# Delete a file.
curl -X DELETE http://sidecart.local/api/v1/gemdrive/files/SWITCHER.TOS
python3 cli/sidecart.py gemdrive rm SWITCHER.TOS
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
| `504 Gateway Timeout` | Synchronous Runner endpoint exceeded its server-side spin-wait deadline (`gateway_timeout`). Per-endpoint deadlines: `runner load` 10 s; `runner unload` 5 s; `runner meminfo`, `runner adv/meminfo`, and each `runner adv/load` chunk 1 s. |

## Error code vocabulary

Clients can switch on `code` reliably. All defined symbols:

`bad_request`, `bad_path`, `bad_query`, `name_too_long`, `not_found`,
`is_directory`, `is_file`, `conflict`, `length_required`,
`payload_too_large`, `range_invalid`, `bad_json`, `unprocessable`,
`unsupported_media`, `method_not_allowed`, `busy`, `disk_error`,
`internal_error`, `runner_inactive`, `gateway_timeout`, `no_snapshot`,
`wrong_hook`, `ram_overflow`, `pexec_failed`, `mfree_failed`,
`program_already_loaded`, `no_program_loaded`.

Runner-specific codes (see *Runner mode* below):
- `runner_inactive` — the user didn't pick `[U]` at boot.
- `busy` — another foreground Runner command is in flight. Every
  m68k-bound foreground verb gates on this lock: `run`, `cd`,
  `res`, `meminfo`, `load`, `exec`, and `unload`. `reset` and the
  entire Advanced surface (`adv/*`) intentionally skip the gate
  because escaping wedged state is the whole point.
- `gateway_timeout` — the Atari ST didn't reply within the
  endpoint's spin-wait deadline (10 s for `runner load`, 5 s for
  `runner unload`, 1 s for `runner meminfo` / `runner adv/meminfo`
  / per-chunk `runner adv/load`).
- `no_snapshot` — the m68k handshake completed but no snapshot was
  recorded (should not happen in practice).
- `wrong_hook` — Advanced Runner command requires the VBL hook (`$70`)
  but `ADV_HOOK_VECTOR` is set to `etv_timer` (`$400`).
- `ram_overflow` — `runner adv load`: the requested target range
  `[address, address+size)` doesn't fit inside `[membottom, phystop)`.

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

### `GET /api/v1/gemdrive/volume` — SD card capacity

**Success** (`200`):
```json
{ "ok": true, "total_b": 8589934592, "free_b": 1073741824, "fs_type": "FAT32" }
```

`fs_type` is one of `FAT12`, `FAT16`, `FAT32`, `EXFAT`, or
`UNKNOWN` (the SD card mounted but FatFs reported a filesystem
type the firmware doesn't have a string for).

**Errors:** `503 busy` if the SD card is not mounted.

**`curl`**:
```sh
curl http://sidecart.local/api/v1/gemdrive/volume
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive volume
```

---

### `GET /api/v1/gemdrive/files?path=<rel>` — list folder

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
curl 'http://sidecart.local/api/v1/gemdrive/files?path=/games'
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive ls /games
```

---

### `GET /api/v1/gemdrive/files/<rel>` — download file

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
curl -o SWITV310.TOS http://sidecart.local/api/v1/gemdrive/files/SWITV310.TOS
# Resume from a partial download:
curl -C - -o SWITV310.TOS http://sidecart.local/api/v1/gemdrive/files/SWITV310.TOS
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive get SWITV310.TOS
python3 cli/sidecart.py gemdrive get SWITV310.TOS -r          # resume
python3 cli/sidecart.py gemdrive get SWITV310.TOS local.tos   # custom local name
```

---

### `PUT /api/v1/gemdrive/files/<rel>?overwrite=0|1` — upload file

Body is the raw file bytes. `Content-Length` is required.
`Content-Length: 0` is legal (creates an empty file or truncates an
existing one when `overwrite=1`). `overwrite=` defaults to `0`; any
value other than `0` or `1` returns `400 bad_query`.

Body cap: **4 MB** (`HTTP_MAX_UPLOAD_BYTES`). Larger → `413`.

**Success:**
- `201 Created` on a new file (carries
  `Location: /api/v1/gemdrive/files/<rel>`).
- `200 OK` on overwrite.

```json
{ "ok": true, "path": "/SWITV310.TOS", "size": 57436 }
```

**Errors:** `400 bad_path` / `name_too_long` / `bad_query`,
`404 not_found` (parent folder missing), `409 conflict` (file
exists and `overwrite=0`, or the target path is the drive
root), `409 is_directory` (target path is a directory),
`411 length_required`, `413 payload_too_large`, `503 busy`.

If the client closes the connection before `Content-Length` bytes
arrive — or FatFs returns an error mid-write — the partially-written
file is deleted before the response is sent.

**`curl`**:
```sh
curl --upload-file ./SWITV310.TOS \
     'http://sidecart.local/api/v1/gemdrive/files/SWITV310.TOS?overwrite=1'
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive put SWITV310.TOS              # default REMOTE = basename
python3 cli/sidecart.py gemdrive put SWITV310.TOS -f           # overwrite
python3 cli/sidecart.py gemdrive put local.tos REMOTE.TOS -f
```

---

### `DELETE /api/v1/gemdrive/files/<rel>` — delete file

**Success** (`204 No Content`, no body).

**Errors:** `400 bad_path`, `404 not_found`, `404 is_directory`
(path is a folder — use `DELETE /api/v1/gemdrive/folders/<rel>`),
`409 conflict` (file is open elsewhere or marked read-only).

**`curl`**:
```sh
curl -X DELETE http://sidecart.local/api/v1/gemdrive/files/STALE.TXT
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive rm STALE.TXT
```

---

### `POST /api/v1/gemdrive/files/<rel>/rename` — rename / move file

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
     http://sidecart.local/api/v1/gemdrive/files/OLD.TXT/rename
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive mv OLD.TXT NEW.TXT
```

---

### `POST /api/v1/gemdrive/folders/<rel>` — create folder

**Success** (`201 Created`, carries `Location: /api/v1/gemdrive/folders/<rel>`):
```json
{ "ok": true, "path": "/SUB" }
```

**Errors:** `400 bad_path` / `name_too_long`, `404 not_found`
(parent folder missing), `409 conflict` (folder already exists or
root).

**`curl`**:
```sh
curl -X POST http://sidecart.local/api/v1/gemdrive/folders/SUB
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive mkdir SUB
```

---

### `DELETE /api/v1/gemdrive/folders/<rel>` — delete folder

**Success** (`204 No Content`).

**Errors:** `400 bad_path`, `404 not_found`, `404 is_file` (path is
a regular file — use `DELETE /api/v1/gemdrive/files/<rel>`),
`409 conflict` (folder is non-empty, locked open, read-only, or is
root).

**`curl`**:
```sh
curl -X DELETE http://sidecart.local/api/v1/gemdrive/folders/SUB
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive rmdir SUB
```

---

### `POST /api/v1/gemdrive/folders/<rel>/rename` — rename / move folder

Mirrors the file rename. Body: `{"to":"<rel>"}`. Cycle detection:
renaming a folder into its own descendant returns `422 unprocessable`.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"to":"/NEWNAME"}' \
     http://sidecart.local/api/v1/gemdrive/folders/OLDNAME/rename
```

**`sidecart`**:
```sh
python3 cli/sidecart.py gemdrive mvdir OLDNAME NEWNAME
```

---

## Runner mode

Runner mode is a foreground execution loop that runs on the Atari
ST instead of the GEMDRIVE-only firmware. The user picks `[U]` at
the setup terminal to launch it; the m68k Runner stays in a poll
loop reading commands from the cartridge sentinel, while GEMDRIVE
keeps servicing TOS file I/O so programs you launch can use the
emulated drive normally.

All Runner endpoints live under `/api/v1/runner/`. The endpoints
fall into three behavioural buckets:

- **Status reads** — `GET /api/v1/runner` and
  `GET /api/v1/runner/adv` are pure RP-side state: they always
  return `200 OK` with the current `active` flag (and, for
  `/runner/adv`, `installed` + `hook_vector`). They never block
  and never return `runner_inactive` — when Runner mode is off
  they just report `active: false`.

- **Foreground commands via the m68k Runner poll loop** — `run`,
  `cd`, `res`, `meminfo`, and the Pexec lifecycle verbs `load` /
  `exec` / `unload`. `run` / `cd` / `res` / `exec` are
  fire-and-forget (`202 Accepted`); `meminfo` / `load` / `unload`
  are synchronous. Every one of these gates on `409
  runner_inactive` when `[U]` wasn't picked, and on `503 busy`
  (with `Retry-After: 1`) when another foreground command is
  already in flight.

- **VBL-ISR-driven commands** — `reset` plus the entire
  `/api/v1/runner/adv/*` surface. These ride the m68k's VBL
  ISR (`$70`, or `$400` if `ADV_HOOK_VECTOR = etv_timer` in the
  setup menu). They return `409 runner_inactive` when `[U]`
  wasn't picked, but **do not** gate on the busy lock —
  escaping wedged state is their job. `adv jump` and `adv load`
  additionally require the VBL hook specifically (`409
  wrong_hook` otherwise); `reset` and `adv meminfo` work on
  either vector.

In addition to the foreground surface, Epic 04 adds an **Advanced
Runner** layer at `/api/v1/runner/adv/...` whose handlers run from
inside the m68k's VBL ISR (or `etv_timer`, depending on the setup
menu's `ADV_HOOK_VECTOR` choice). VBL-driven commands keep working
when the foreground poll loop is blocked — wedged programs, bombs
already painted, traps disabled. See the *Advanced Runner* section
below the foreground endpoints.

> **Shell-quoting note** — several Advanced commands accept a
> `$hex` legacy form (e.g. `$78000`, equivalent to `0x78000`). Bash
> and zsh expand `$78000` as a variable reference (unset → empty →
> silently dropped from argv); always single-quote the `$hex` form
> on the command line (`'$78000'`) or use `0xhex`, which needs no
> quoting.

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
  "loaded_basepage": null,
  "last_load_errno": null,
  "last_started_at_ms": 12345,
  "last_finished_at_ms": 13002
}
```

`last_command` is one of `null`, `RESET`, `EXECUTE`, `CD`, `RES`,
`MEMINFO`, `JUMP`, `LOAD`, `PEXEC_LOAD`, `PEXEC_EXEC`, or
`PEXEC_UNLOAD`. The `last_cd_errno` / `last_res_errno` fields
are `null` unless the most-recent command was a `CD` or `RES`
respectively. `JUMP` / `LOAD` reflect the Advanced commands of
the same name (see *Advanced Runner* below); `PEXEC_LOAD` /
`PEXEC_EXEC` / `PEXEC_UNLOAD` are the load / exec / unload
lifecycle verbs (see [Pexec lifecycle](#pexec-lifecycle-load--exec--unload)).
`loaded_basepage` is the cached basepage pointer when a program
is currently loaded (via `runner load`), `null` otherwise.
`last_load_errno` carries the GEMDOS errno of the most-recent
failed `load` or `unload` (`null` on success).

**`sidecart`**:
```sh
python3 cli/sidecart.py runner status        # human form
python3 cli/sidecart.py runner status --json # raw envelope
```

---

### `POST /api/v1/runner/reset` — cold-reset the ST

Fires `RUNNER_ADV_CMD_RESET` at the cartridge sentinel; the m68k
Runner's VBL hook (installed at `$70` or `$400` per the
`ADV_HOOK_VECTOR` aconfig setting) sees it from inside the ISR
and jumps through the reset vector at `$4.w`. Riding the VBL ISR
(rather than the foreground poll loop, which earlier revisions
used) lets `reset` escape wedged programs that have hung the
Runner foreground — infinite loops, bombs already painted, traps
disabled.

The firmware-mode commit lives on the Pico, not the ST, so the
Runner re-launches itself on the next `gemdrive_init` HELLO
handshake — no operator action needed, no detour through the
setup menu. Stale RP-side state (busy lock, cwd mirror, last-cd
/ last-res errno, any `loaded_basepage` from a prior `runner
load`) is cleared by the HELLO message at re-entry.

Returning to the setup menu requires power-cycling the ST **and**
hard-resetting the Pico (or re-flashing it).

Intentionally skips the busy gate — busy state is exactly what
this verb exists to escape. Returns `409 runner_inactive` only
when `[U]` was never picked.

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
python3 cli/sidecart.py runner run RUNME.TOS                 # if cwd is /GAMES/ARKANOID
python3 cli/sidecart.py runner run PROG.TOS -v --file foo    # cmdline = "-v --file foo"
```

Everything after `REMOTE` is captured verbatim into `cmdline`,
including leading dashes — no `--` separator is needed. (If you
do pass `--`, it lands in `cmdline` literally.)

Common error codes: `404 not_found` (program file doesn't exist),
`400 bad_path` (rejected name), `400 bad_request` (cmdline too
long), `409 runner_inactive`, `503 busy`.

---

### Pexec lifecycle: `load` / `exec` / `unload`

Three verbs that split GEMDOS Pexec mode 0 ("load and go") into
separate steps so a workstation can drive ST programs the way an
interactive debugger would: load once, exec many times, unload
when done. The corresponding Pexec mode for each verb is:

| Verb | GEMDOS mode | Effect |
| --- | --- | --- |
| `load` | `Pexec(3)` | Load file → relocate → return basepage. Program is in m68k RAM but has not run. |
| `exec` | `Pexec(4)` | "Just go" — execute the program at the previously-loaded basepage. Does **not** free the basepage on exit, so re-exec on the same loaded program is valid. |
| `unload` | `Mfree(basepage)` | Release the basepage back to GEMDOS. Required before another `load` (strict-refuse — see below). |

**RP-side state**: a single basepage slot is held server-side
between `load` and `unload`. Surfaced in `GET /api/v1/runner` as:
- `loaded_basepage` — the cached basepage pointer, or `null` if
  no program is loaded.
- `last_load_errno` — `null` on success, the negative GEMDOS
  errno if the most recent `load` (or `unload`) returned an
  error.

**Strict-refuse semantics**: `load` while a program is already
loaded returns `409 program_already_loaded` — the caller must
`unload` (or restart Runner mode) before submitting a new load.
This keeps the m68k from holding orphan basepages the RP forgot
about.

#### `POST /api/v1/runner/load` — `Pexec(3)` load only

Body: `{"path":"<rel>", "cmdline":"<≤127 chars>"}` — same shape
as `/runner/run`. **Synchronous** — the response includes the
basepage on success; failure returns the GEMDOS errno
synchronously instead of via a follow-up `runner status`.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"path":"/HELLODBG.TOS","cmdline":""}' \
     http://sidecart.local/api/v1/runner/load
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner load /HELLODBG.TOS
python3 cli/sidecart.py runner load PROG.TOS -v --file foo
```

Response 200:
```json
{ "ok": true, "loaded": true, "basepage": 65566 }
```

Common error codes: `200 OK` (success — basepage in body), `422
pexec_failed` (GEMDOS errno in `gemdos_errno` field — file too
big, out of memory, etc.), `404 not_found`, `409
runner_inactive`, `409 program_already_loaded`, `503 busy`,
`504 gateway_timeout` (m68k didn't respond within 10 s).

#### `POST /api/v1/runner/exec` — `Pexec(4)` just-go

Empty body. Executes the program previously loaded via
`/runner/load`. **Fire-and-forget** like `/runner/run` — the
exit code arrives asynchronously and surfaces on the next
`/runner/status` as `last_exit_code`. The basepage stays loaded
after exit, so calling `exec` again re-runs the same program.

**`curl`**:
```sh
curl -X POST http://sidecart.local/api/v1/runner/exec
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner exec
```

Response 202:
```json
{ "ok": true, "accepted": true }
```

Common error codes: `202 Accepted`, `409 no_program_loaded` (no
prior `load`), `409 runner_inactive`, `503 busy`.

#### `POST /api/v1/runner/unload` — release the basepage

Empty body. Issues GEMDOS `Mfree` on the loaded basepage and
clears the RP-side state. **Synchronous** — the response
confirms the freed basepage on success.

**`curl`**:
```sh
curl -X POST http://sidecart.local/api/v1/runner/unload
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner unload
```

Response 200:
```json
{ "ok": true, "unloaded": true, "basepage": 65566 }
```

Common error codes: `200 OK`, `422 mfree_failed` (GEMDOS errno
in `gemdos_errno` — rare, typically means the basepage was
already freed), `409 no_program_loaded`, `409 runner_inactive`,
`503 busy`, `504 gateway_timeout`.

#### Full lifecycle example

```sh
python3 cli/sidecart.py runner load /HELLODBG.TOS    # → basepage 0x...
python3 cli/sidecart.py runner status                # loaded_basepage shows the pointer
python3 cli/sidecart.py runner exec                  # runs, "Hello, world!" output
python3 cli/sidecart.py runner exec                  # runs AGAIN — re-exec is supported
python3 cli/sidecart.py runner unload                # frees the basepage
python3 cli/sidecart.py runner status                # loaded_basepage: null
python3 cli/sidecart.py runner exec                  # → 409 no_program_loaded
```

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
or "not a directory"), `409 runner_inactive`, `503 busy`.

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
runner_inactive`, `503 busy`.

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

Common error codes: `409 runner_inactive`, `503 busy`,
`504 gateway_timeout`.

---

## Advanced Runner

VBL-ISR-driven command surface (Epic 04). Handlers run from inside
the m68k's level-4 autovector at `$70` (or `$400` if you flipped
`ADV_HOOK_VECTOR` to `etv_timer` in the setup menu), so they keep
firing even when the foreground poll loop is wedged. Two of the
three POSTs (`adv jump`, `adv load`) require the VBL hook
specifically — they patch the trap frame's saved PC to `rte` to a
new address, and the trap-frame layout for `etv_timer` is past
TOS' MFP scratch and TOS-version-fragile to walk. They return
`409 wrong_hook` when the hook is wrong; `adv meminfo` works on
either vector.

None of the Advanced endpoints gate on the busy lock — the whole
point is to escape state the foreground can't reach.

Status query:

### `GET /api/v1/runner/adv` — Advanced Runner state

Reports whether the m68k Runner has installed its VBL hook and
which vector it landed on.

**`curl`**:
```sh
curl http://sidecart.local/api/v1/runner/adv
```

**Response (200 OK)**:
```json
{ "ok": true, "active": true, "installed": true, "hook_vector": "vbl" }
```

`hook_vector` is one of `"vbl"`, `"etv_timer"`, or `"unknown"`
(the m68k didn't report — old firmware or no HELLO yet).

**`sidecart`**:
```sh
python3 cli/sidecart.py runner adv status
```

---

### `POST /api/v1/runner/adv/meminfo` — meminfo from inside the VBL ISR

Same wire format as `GET /api/v1/runner/meminfo` — the m68k ships
the 24-byte snapshot back, the RP records it, the response is the
identical JSON shape. The difference is *where* on the m68k the
snapshot is read: this endpoint dispatches the read from inside
the VBL ISR (or `etv_timer`), so it works against wedged programs
the foreground meminfo can't reach. Synchronous, 1 s timeout.

**`curl`**:
```sh
curl -X POST http://sidecart.local/api/v1/runner/adv/meminfo
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner adv meminfo
```

Common error codes: `409 runner_inactive`, `504 gateway_timeout`,
`500 no_snapshot`.

---

### `POST /api/v1/runner/adv/jump` — `rte` to a user address

Body: `{"address":"<int>"}`. Address is a JSON string in decimal
or `0x`-hex form (the CLI normalises any input — decimal, `$hex`,
`0xhex` — to `0x`-hex before posting). Validation: 24-bit
(`$0..$FFFFFF`) and even (m68k 68000 instruction alignment).

The handler stashes the address in shared-var slot 17 and fires
`RUNNER_ADV_CMD_JUMP`. The next VBL the m68k's adv handler reads
the slot, patches its own trap-frame's saved PC, and `rte`s. The
hook stays installed; the sentinel is cleared by the chunk-done
chandler so subsequent VBLs just chain.

Fire-and-forget (`202 Accepted`) — there is no completion event
the m68k sends back beyond the no-payload `RUNNER_ADV_CMD_DONE_JUMP`
that triggers the sentinel clear.

**VBL hook only.** Returns `409 wrong_hook` when
`ADV_HOOK_VECTOR = etv_timer`.

**`curl`**:
```sh
curl -X POST -H 'Content-Type: application/json' \
     -d '{"address":"0xFA1C00"}' \
     http://sidecart.local/api/v1/runner/adv/jump
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner adv jump 0xFA1C00
python3 cli/sidecart.py runner adv jump '$FA1C00'   # legacy hex — must single-quote
python3 cli/sidecart.py runner adv jump 16384       # decimal
```

Common error codes: `400 bad_request` (odd, out-of-24-bit, or
unparseable address), `409 runner_inactive`, `409 wrong_hook`.

---

### `POST /api/v1/runner/adv/load` — stream a workstation file into m68k RAM

Query: `?address=<int>[&size=<int>]`. Body: raw bytes
(`Content-Type` ignored). The handler drains the body into APP_FREE
in 8 KB chunks (byte-pair-swapped so the m68k's word reads land
native), and after each fill it dispatches a chunk via
`RUNNER_ADV_CMD_LOAD_CHUNK`; the m68k VBL handler copies the chunk
to its target and acks. The HTTP request returns `200 OK`
synchronously when the last chunk lands.

Validation:
- Address must be even and ≤ 24 bits.
- The full range `[address, address+size)` must fit in
  `[membottom, phystop)` per the most recent meminfo snapshot.
  The handler triggers `RUNNER_ADV_CMD_MEMINFO` itself if no
  snapshot is cached. → `400 ram_overflow` if it doesn't fit.
- Body Content-Length ≤ 4 MB. → `413 payload_too_large`.
- `size`, when present, caps the upload (`min(file_size, size)`).
  Trailing bytes past the cap are silently dropped (cap-and-truncate).

**VBL hook only.** Returns `409 wrong_hook` when
`ADV_HOOK_VECTOR = etv_timer`.

**Mid-transfer failure leaves partial bytes in m68k RAM.** A
`504 gateway_timeout` mid-stream means some chunks already landed;
caller's responsibility to retry or repair.

**`curl`**:
```sh
curl -X POST --data-binary @./game.prg \
     'http://sidecart.local/api/v1/runner/adv/load?address=0x40000'
# With a size cap:
curl -X POST --data-binary @./game.prg \
     'http://sidecart.local/api/v1/runner/adv/load?address=0x40000&size=4096'
```

**Response (200 OK)**:
```json
{ "ok": true, "loaded": true, "address": 262144, "bytes": 12345 }
```

**`sidecart`**:
```sh
python3 cli/sidecart.py runner adv load ./game.prg 0x40000
python3 cli/sidecart.py runner adv load ./game.prg 0x40000 4096   # cap to 4 KB
python3 cli/sidecart.py runner adv load ./game.prg '$40000'        # legacy hex — must single-quote
```

Common error codes: `400 bad_request` (odd / out-of-range address,
bad size), `400 ram_overflow`, `409 runner_inactive`,
`409 wrong_hook`, `413 payload_too_large`, `504 gateway_timeout`.

---

## Debug traces

A high-bandwidth, zero-CPU-overhead path the Atari ST (and any
TOS / PRG it runs) can use to stream diagnostic bytes back to a
workstation. The capture rides on the existing ROM3 protocol
pipeline: every read in `$FBFF00..$FBFFFF` is filtered on the
RP side, and the byte (`address & 0xFF`) is appended to a small
ring drained by HTTP and / or USB CDC consumers.

**m68k emit form (the public ABI)**:

```c
#define DEBUG_BASE 0xFBFF00UL
(void)*(volatile char *)(DEBUG_BASE + c);   // emit byte c
```

One cartridge cycle per byte. The 8-bit read result is undefined
and MUST be discarded.

The capture is gated on the firmware-mode flag — it goes live
the moment the user picks `[U]` / `[E]` / `[F]` in the setup
menu and the device commits to firmware mode. Pre-commit reads
in the debug window are dropped at the RP filter so menu-mode
activity never pollutes the diagnostic stream.

Two consumers can read the captured stream in parallel without
stealing bytes from each other: an HTTP `tail -f`-style
streamer and a USB CDC sink. Each holds its own logical cursor
into the ring; both see the full byte stream independently.

### `GET /api/v1/debug` — diagnostics envelope

Reports the firmware-mode flag and ring stats. Does NOT consume
bytes; safe to poll at any cadence.

**Response 200**:
```json
{
  "ok": true,
  "firmware_mode": true,
  "ring_used": 8192,
  "ring_capacity": 8192,
  "bytes_dropped": 0,
  "usbcdc_attached": true,
  "usbcdc_dropped": 0
}
```

| Field | Meaning |
| --- | --- |
| `firmware_mode` | `true` once `[U]` / `[E]` / `[F]` has committed; until then, debug emits are dropped at the RP filter. |
| `ring_used` | Bytes currently in the producer's ring (capped at `ring_capacity`). |
| `ring_capacity` | Ring size (8192 today). |
| `bytes_dropped` | Producer-side drops. Always 0 in the current design (the producer overwrites; per-consumer drops are reported separately). |
| `usbcdc_attached` | `true` iff a host has the CDC port open with DTR asserted. |
| `usbcdc_dropped` | Cumulative bytes lost on the USB CDC consumer's cursor since boot — sum of (a) bytes emitted while no host was attached, and (b) in-session drops where the host's TX FIFO stalled. |

**`curl`**:
```sh
curl http://sidecart.local/api/v1/debug
```

**`sidecart`**:
```sh
python3 cli/sidecart.py debug status
```

### `GET /api/v1/debug/log` — chunked byte stream

Long-lived `tail -f` shape. The response uses HTTP/1.1
`Transfer-Encoding: chunked` and `Content-Type:
application/octet-stream` (raw bytes, no encoding). The
connection stays open until the client disconnects, the device
reboots, or the network drops.

Each connected client gets its own cursor, snapshotted at the
moment the connection opens — clients see only bytes emitted
*from that point forward*, not the ring's stale tail.

**`curl`**:
```sh
curl --no-buffer http://sidecart.local/api/v1/debug/log
```

**`sidecart`**:
```sh
python3 cli/sidecart.py debug tail
```

Errors: `404 not_found` if the route is unavailable on the
build (shouldn't happen — included unconditionally).

### USB CDC alternative

After flashing, the device also exposes a USB CDC interface.
Plug a USB cable into the Pico and the workstation gets a
serial port (`/dev/tty.usbmodem*` on macOS, `/dev/ttyACM*` on
Linux, `COM*` on Windows). The same debug bytes the HTTP tail
streams will land on that port, byte-exact:

```sh
screen /dev/tty.usbmodem*  115200          # baud is informational
```

The CDC port is dedicated to debug bytes — the firmware's own
DPRINTF diagnostics go to the UART debug header, not the CDC
port. (Toggle `_DEBUG=1` in the build to enable UART DPRINTF.)

### Verifying the path end-to-end

The repo ships a tiny ST test program at
`target/atarist/test/hello-debug/`. Build it (`./build.sh`),
upload, run, and watch one of the consumers:

```sh
# Build, upload, run.
target/atarist/test/hello-debug/build.sh
python3 cli/sidecart.py gemdrive put target/atarist/test/hello-debug/dist/HELLODBG.TOS /
python3 cli/sidecart.py runner run /HELLODBG.TOS

# In another shell, watch the bytes via either transport:
python3 cli/sidecart.py debug tail              # → "Hello, world!\n" × 1000
# or
screen /dev/tty.usbmodem*  115200
```

`runner status` should report `last_exit_code: 0` once the
program exits.

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
