; SidecarTridge Multi-device DevOps — Runner module (Epic 03)
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; The cartridge image places this module at offset $1C00 inside the
; 8 KB cartridge code section (RUNNER_BLOB = $FA1C00) via
; target/atarist/src/devops.ld. main.s reaches it through
; `runner_function: jmp RUNNER_BLOB` when CMD_START_RUNNER (5)
; arrives at the sentinel — i.e. the user pressed [U] in the setup
; terminal.
;
; In v1 (Epic 03 / S1) the Runner runs **directly from cartridge ROM**
; — no relocation. PC-relative addressing is used everywhere so the
; same `runner.s` will be drop-in compatible with the future
; relocate-to-RAM install pattern (mirroring gemdrive.s) when needed.
;
; Responsibilities (S1 skeleton):
;   1. Clear the screen.
;   2. Paint a banner.
;   3. Publish the RUNNER_HELLO magic + protocol version into the
;      Runner shared sub-region so the RP-side `GET /api/v1/runner`
;      handshake reports `active=true`.
;   4. Loop forever (active foreground poll). Subsequent stories
;      add the actual command dispatch (RESET / EXECUTE / CD).

	section text

; ---------------------------------------------------------------
; Constants — must match the RP-side authority (chandler.h /
; rp/src/include/runner.h) and main.s.
; ---------------------------------------------------------------

ROM4_ADDR			equ $FA0000
SHARED_BLOCK_ADDR		equ (ROM4_ADDR + $2000)		; $FA2000
CMD_MAGIC_SENTINEL_ADDR		equ SHARED_BLOCK_ADDR		; $FA2000
APP_FREE_ADDR			equ (ROM4_ADDR + $2300)		; $FA2300

; Runner sub-region inside APP_FREE. The base is positioned past
; GEMDRIVE's existing allocation (highest offset $162C; we leave
; ~512 B of slack before the Runner block).
RUNNER_BASE_OFFSET		equ $1800
RUNNER_BASE			equ (APP_FREE_ADDR + RUNNER_BASE_OFFSET)
RUNNER_PATH			equ (RUNNER_BASE + $000)	; 128 B
RUNNER_CMDLINE			equ (RUNNER_BASE + $080)	; 128 B
RUNNER_LAST_STATUS		equ (RUNNER_BASE + $100)	; 4 B
RUNNER_DONE_FLAG		equ (RUNNER_BASE + $104)	; 4 B
RUNNER_CWD			equ (RUNNER_BASE + $108)	; 128 B
RUNNER_HELLO			equ (RUNNER_BASE + $188)	; 4 B  ('RNV1')
RUNNER_PROTO_VER		equ (RUNNER_BASE + $18C)	; 4 B  (u16 min | u16 max)

RUNNER_HELLO_MAGIC		equ $524E5631	; 'RNV1' big-endian
RUNNER_PROTO_VERSION		equ $00010001	; min=1, max=1

; Sentinel commands that main.s already polls. The Runner clears the
; sentinel back to NOP after taking control so check_commands doesn't
; re-trigger.
CMD_NOP				equ 0

; GEMDOS / XBIOS opcodes used here.
GEMDOS_Cconws			equ 9

; ---------------------------------------------------------------
; Entry point — offset 0 of the runner blob. Reached via
; `jmp RUNNER_BLOB` from main.s's check_commands dispatch.
; ---------------------------------------------------------------

runner_entry:
	; The cartridge address space ($FA0000-$FAFFFF) is read-only
	; from the m68k — the cartridge port has no write strobe and
	; any store there bus-errors (two bombs). The Runner therefore
	; never writes to the shared region directly. The RP-side
	; cmdRunner handler flips an in-memory `runner_active` flag the
	; moment it sends DISPLAY_COMMAND_START_RUNNER, so
	; `GET /api/v1/runner` knows the user chose Runner mode without
	; a handshake from the m68k. Future stories will push richer
	; state (exit codes, cwd) up to the RP via the cartridge
	; protocol (send_sync with APP_RUNNER command IDs), where the
	; RP-side runner_command_cb stashes it in RP RAM.

	; --- Step 1: clear screen via VT52 ESC E ---
	pea	vt52_clear(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; --- Step 2: paint banner ---
	pea	banner_text(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; --- Step 3: active poll loop. S1 skeleton — no command
	; dispatch yet, just spin. RUNNER_RESET / EXECUTE / CD land in
	; later stories. ---
runner_poll_loop:
	bra.s	runner_poll_loop

; ---------------------------------------------------------------
; Read-only data (lives in cartridge ROM alongside the code).
; ---------------------------------------------------------------

vt52_clear:
	dc.b	27, "E", 0
	even

banner_text:
	dc.b	13, 10
	dc.b	"  DevOps Runner ", 13, 10
	dc.b	"  -----------------------------------", 13, 10
	dc.b	"  [Ready] - waiting for commands", 13, 10
	dc.b	0
	even

; Trailing sentinel: keeps the cartridge-image last-non-zero-byte on
; an odd offset so firmware.py's "trim trailing zeros + assert even
; length" pass succeeds even when the banner text ends with a NUL.
runner_blob_end:
	dc.w	$FFFF
