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

; Sentinel commands. main.s' framework values (CMD_NOP=0,
; CMD_RESET=1, CMD_BOOT_GEM=2, CMD_TERMINAL=3, CMD_START=4,
; CMD_START_RUNNER=5) live in the [0..15] range; Runner-namespace
; commands live at $0500 + N to avoid collisions.
CMD_NOP				equ 0
APP_RUNNER			equ $0500
RUNNER_CMD_RESET		equ ($01 + APP_RUNNER)	; cold reset
RUNNER_CMD_EXECUTE		equ ($02 + APP_RUNNER)	; Pexec mode 0
RUNNER_CMD_CD			equ ($03 + APP_RUNNER)	; Dsetpath
; m68k -> RP report commands (sent via send_sync from the Runner).
RUNNER_CMD_DONE_EXECUTE		equ ($82 + APP_RUNNER)	; payload: i32 exit code
RUNNER_CMD_DONE_CD		equ ($83 + APP_RUNNER)	; payload: i32 GEMDOS errno
RUNNER_CMD_DONE_HELLO		equ ($84 + APP_RUNNER)	; no payload — runner entered loop

; Wait between the RUNNER_RESET sentinel sighting and the actual
; reset trampoline — gives any in-flight cartridge bus traffic time
; to drain. Mirrors main.s' PRE_RESET_WAIT.
PRE_RESET_WAIT			equ $FFFFF

; Constants required by inc/sidecart_macros.s when send_sync expands.
; main.s defines these too; we need our own local copies because
; runner.s is its own assembly unit.
RANDOM_TOKEN_ADDR		equ $FA2004
RANDOM_TOKEN_SEED_ADDR		equ $FA2008
RANDOM_TOKEN_POST_WAIT		equ $1
ROMCMD_START_ADDR		equ $FB0000
CMD_MAGIC_NUMBER		equ $ABCD
CMD_RETRIES_COUNT		equ 3
CMD_SET_SHARED_VAR		equ 1
COMMAND_TIMEOUT			equ $0000FFFF
COMMAND_WRITE_TIMEOUT		equ COMMAND_TIMEOUT
_dskbufp			equ $4C6

; GEMDOS / XBIOS opcodes used here.
GEMDOS_Cconws			equ 9
GEMDOS_Dsetdrv			equ $E
GEMDOS_Dsetpath			equ $3B
GEMDOS_Pexec			equ $4B
PE_LOAD_GO			equ 0	; Pexec mode 0: load + go, returns

; Shared-variable slot 12 (drive number) is published by the RP-side
; handleGemdriveHello during gemdrive_init. The m68k's TOS process was
; created at GEMDOS init time — *before* install_entry ran — so its
; current drive was inherited from the boot ROM's _bootdev (typically
; A:), not from the value install_entry later wrote. The Runner forces
; a Dsetdrv to the emulated drive before Pexec so gemdrive's
; .Pexec/detect_emulated_drive macro sees a current drive that matches
; SHARED_VAR_DRIVE_NUMBER and stays on the GEMDRIVE-handled path
; instead of chaining to TOS native (which would EFILNF on C:\).
SHARED_VARIABLES_ADDR		equ $FA2010
SHARED_VAR_DRIVE_NUMBER		equ 12
DRIVE_NUMBER_ADDR		equ (SHARED_VARIABLES_ADDR + SHARED_VAR_DRIVE_NUMBER*4)

; main.s' send_sync_command_to_sidecart lives ~6 KB away from this
; module's BSR sites; that fits BSR.W's ±32 KB range, but vlink emits
; an absolute relocation for cross-module references and tries to
; squeeze the 32-bit absolute address into the BSR.W's 16-bit slot,
; which fails. Substitute a private send_sync macro that uses JSR
; (absolute long) instead — same semantics, just 4 bytes more per
; call site.
	xref	send_sync_command_to_sidecart

runner_send_sync	macro
	move.w	#CMD_RETRIES_COUNT, d7
.\@retry:
	movem.l	d1-d7, -(sp)
	moveq.l	#\2, d1
	move.w	#\1, d0
	jsr	send_sync_command_to_sidecart
	movem.l	(sp)+, d1-d7
	tst.w	d0
	beq.s	.\@ok
	dbf	d7, .\@retry
.\@ok:
	endm

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

	; --- Step 0: known cwd baseline. Whether we landed here from a
	; cold boot, a runner reset, or the [U] menu pick, force the
	; current drive onto the GEMDRIVE-emulated drive and Dsetpath the
	; cwd back to root. The RP-side mirror clears its cwd on HELLO
	; below, so relative `runner cd` / `runner run` always resolve
	; from the same baseline on both sides. ---
	move.l	DRIVE_NUMBER_ADDR, d3
	move.w	d3, -(sp)
	move.w	#GEMDOS_Dsetdrv, -(sp)
	trap	#1
	addq.l	#4, sp

	pea	root_path(pc)
	move.w	#GEMDOS_Dsetpath, -(sp)
	trap	#1
	addq.l	#6, sp

	; Tell the RP a fresh Runner session just started so it clears
	; any stale busy lock / cwd mirror that survived a physical or
	; cold reset (where the RP itself didn't see the reset event).
	runner_send_sync RUNNER_CMD_DONE_HELLO, 0

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

	; --- Step 3: active poll loop. Reads the cartridge sentinel
	; the RP writes to dispatch Runner commands. RUNNER_CMD_EXECUTE
	; / RUNNER_CMD_CD land in S3/S4. ---
runner_poll_loop:
	move.l	CMD_MAGIC_SENTINEL_ADDR, d6
	cmp.l	#RUNNER_CMD_RESET, d6
	beq	runner_reset
	cmp.l	#RUNNER_CMD_EXECUTE, d6
	beq	runner_execute
	cmp.l	#RUNNER_CMD_CD, d6
	beq	runner_cd
	bra.s	runner_poll_loop

; RUNNER_CMD_EXECUTE handler. Reads RUNNER_PATH (NUL-terminated) and
; RUNNER_CMDLINE (TOS length-prefixed) from the Runner sub-region of
; APP_FREE, then calls GEMDOS Pexec mode 0 (load + go). On return,
; the program's exit code is in d0; we ship it to the RP via
; send_sync RUNNER_CMD_DONE_EXECUTE so the RP can update its state
; struct (last_exit_code, busy=false). Then resume polling.
runner_execute:
	; Trace: "[RUN  ] - Launching <path>\r\n". Cconws clobbers d0/d1/d2/a0-a2,
	; but we haven't called Pexec yet so no exit code to preserve.
	pea	text_run(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	pea	RUNNER_PATH.l
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	pea	text_crlf(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; Switch the m68k's current drive to the GEMDRIVE-emulated one so
	; gemdrive's .Pexec drive-detection macro recognises this Pexec
	; as belonging to the emulated drive and dispatches via
	; CMD_PEXEC_CALL. See comment near DRIVE_NUMBER_ADDR above.
	move.l	DRIVE_NUMBER_ADDR, d3
	move.w	d3, -(sp)
	move.w	#GEMDOS_Dsetdrv, -(sp)
	trap	#1
	addq.l	#4, sp

	; Pexec stack frame (top-down after pushes):
	;   [sp+0]   .w  GEMDOS function code ($4B)
	;   [sp+2]   .w  mode (0 = PE_LOAD_GO)
	;   [sp+4]   .l  fname (path)
	;   [sp+8]   .l  cmdline
	;   [sp+12]  .l  envstring (NULL = inherit)
	clr.l	-(sp)			; envstring = NULL (inherit)
	move.l	#RUNNER_CMDLINE, -(sp)	; cmdline pointer (TOS-format, length-prefixed)
	move.l	#RUNNER_PATH, -(sp)	; fname pointer (NUL-terminated)
	move.w	#PE_LOAD_GO, -(sp)
	move.w	#GEMDOS_Pexec, -(sp)
	trap	#1
	lea	16(sp), sp

	; Stash exit code on the stack — Cconws below will trash d0.
	move.l	d0, -(sp)

	; Trace: "[EXIT ] - <path> terminated\r\n".
	pea	text_exit(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	pea	RUNNER_PATH.l
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	pea	text_terminated(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; Recover exit code and ship it to the RP.
	move.l	(sp)+, d3
	runner_send_sync RUNNER_CMD_DONE_EXECUTE, 4
	bra	runner_poll_loop

; RUNNER_CMD_CD handler. Reads RUNNER_PATH (NUL-terminated) and calls
; GEMDOS Dsetpath ($3B). Returns the GEMDOS errno (0 on success,
; negative on error) to the RP via RUNNER_CMD_DONE_CD. Pre-Dsetdrv
; for the same reason as runner_execute (see DRIVE_NUMBER_ADDR
; comment) — gemdrive's .Dsetpath dispatch checks the current drive
; and chains to TOS native if it doesn't match the emulated drive.
runner_cd:
	; Trace: "[CD   ] - <path>\r\n"
	pea	text_cd(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	pea	RUNNER_PATH.l
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	pea	text_crlf(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; Switch to the emulated drive first.
	move.l	DRIVE_NUMBER_ADDR, d3
	move.w	d3, -(sp)
	move.w	#GEMDOS_Dsetdrv, -(sp)
	trap	#1
	addq.l	#4, sp

	; GEMDOS Dsetpath(path) — d0 = errno.
	move.l	#RUNNER_PATH, -(sp)
	move.w	#GEMDOS_Dsetpath, -(sp)
	trap	#1
	addq.l	#6, sp

	; Ship errno to the RP.
	move.l	d0, d3
	runner_send_sync RUNNER_CMD_DONE_CD, 4
	bra	runner_poll_loop

; Cold reset — same sequence as main.s' .reset. Waits briefly,
; invalidates TOS' memory-system "valid" cookies (so the next boot
; rebuilds RAM tables instead of trusting stale ones), then jumps
; through the reset vector at $00000004.
runner_reset:
	; Trace before the wait so the user sees something even if the
	; reset itself takes a moment.
	pea	text_reset(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	move.l	#PRE_RESET_WAIT, d6
.runner_reset_wait:
	subq.l	#1, d6
	bne.s	.runner_reset_wait

	clr.l	$420.w			; memvalid
	clr.l	$43A.w			; memval2
	clr.l	$51A.w			; memval3
	move.l	$4.w, a0		; reset vector
	jmp	(a0)
	nop

; ---------------------------------------------------------------
; Read-only data (lives in cartridge ROM alongside the code).
; ---------------------------------------------------------------

vt52_clear:
	dc.b	27, "E", 0
	even

; GEMDOS Dsetpath argument — the GEMDRIVE drive root. Backslash
; written as a numeric byte ($5C) so vasm doesn't treat it as a
; string-escape char.
root_path:
	dc.b	$5C, 0
	even

banner_text:
	dc.b	13, 10
	dc.b	"DevOps Runner ", 13, 10
	dc.b	"---------------------------------------", 13, 10
	dc.b	"[READY] - Waiting for commands", 13, 10
	dc.b	0
	even

; Per-command trace strings. Cconws prints these alongside the path
; (read straight from RUNNER_PATH in APP_FREE) so the operator can see
; on the ST screen exactly which command landed and which program ran.
text_reset:
	dc.b	13, 10, "[RESET] - Received Cold Reset command", 13, 10, 0
	even
text_run:
	dc.b	13, 10, "[RUN  ] - Launching ", 0
	even
text_exit:
	dc.b	13, 10, "[EXIT ] - ", 0
	even
text_cd:
	dc.b	13, 10, "[CD   ] - ", 0
	even
text_terminated:
	dc.b	" terminated", 13, 10, 0
	even
text_crlf:
	dc.b	13, 10, 0
	even

; Trailing sentinel: keeps the cartridge-image last-non-zero-byte on
; an odd offset so firmware.py's "trim trailing zeros + assert even
; length" pass succeeds even when the banner text ends with a NUL.
runner_blob_end:
	dc.w	$FFFF
