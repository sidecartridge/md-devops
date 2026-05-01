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
RUNNER_REZ			equ (RUNNER_BASE + $190)	; 4 B (u16 target rez for RUNNER_CMD_RES)

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
RUNNER_CMD_RES			equ ($04 + APP_RUNNER)	; XBIOS Setscreen — rez at RUNNER_REZ
; m68k -> RP report commands (sent via send_sync from the Runner).
RUNNER_CMD_DONE_EXECUTE		equ ($82 + APP_RUNNER)	; payload: i32 exit code
RUNNER_CMD_DONE_CD		equ ($83 + APP_RUNNER)	; payload: i32 GEMDOS errno
RUNNER_CMD_DONE_HELLO		equ ($84 + APP_RUNNER)	; no payload — runner entered loop
RUNNER_CMD_DONE_RES		equ ($85 + APP_RUNNER)	; payload: i32 errno (0 = OK, -1 = mono, -2 = bad rez)

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
GEMDOS_Cconout			equ 2
GEMDOS_Dsetdrv			equ $E
GEMDOS_Dsetpath			equ $3B
GEMDOS_Pexec			equ $4B
XBIOS_Getrez			equ 4
XBIOS_Setscreen			equ 5
XBIOS_Setpalette		equ 6
XBIOS_Vsync			equ 37
PE_LOAD_GO			equ 0	; Pexec mode 0: load + go, returns

; RUNNER_RES errno codes (i32 in RUNNER_CMD_DONE_RES payload).
RUNNER_RES_OK			equ 0
RUNNER_RES_ERR_MONO		equ -1	; current rez is high (mono); request ignored
RUNNER_RES_ERR_BAD		equ -2	; requested rez out of range

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

	; --- Step 1: clear screen + paint banner (single Cconws — the
	; banner_text string leads with VT52 ESC E). ---
	pea	banner_text(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; --- Step 2: active poll loop. Reads the cartridge sentinel
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
	cmp.l	#RUNNER_CMD_RES, d6
	beq	runner_res
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

	; Trace: "\r\n[EXIT <code>] - <path> terminated\r\n" so the operator
	; sees the result without a `runner status` round-trip.
	pea	text_exit_open(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; Print the saved exit code as signed decimal. After the
	; pea+move.w/trap/addq sequence above, sp points back at the
	; saved exit code longword.
	move.l	(sp), d3
	bsr	runner_print_dec_d3

	pea	text_exit_close(pc)
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

; RUNNER_CMD_RES handler. Stateless screen-resolution change for
; colour monitors. The RP wrote the desired rez (0=low, 1=med) as a
; u16 at RUNNER_REZ. We read XBIOS Getrez first: if it returns 2
; (high/mono) we ignore the request and report errno -1, otherwise
; we call XBIOS Setscreen(-1, -1, rez) and report 0.
runner_res:
	; Trace: "[RES  ] - <name>\r\n". The mnemonic gets resolved
	; later by glancing at the rez byte — for now print a generic
	; tag, the requested code is visible in `runner status`.
	pea	text_res(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; XBIOS Getrez → d0.w = 0 (low) | 1 (med) | 2 (high/mono).
	move.w	#XBIOS_Getrez, -(sp)
	trap	#14
	addq.l	#2, sp

	cmp.w	#2, d0
	bne.s	.runner_res_apply

	; Mono — refuse, errno -1.
	moveq	#RUNNER_RES_ERR_MONO, d3
	bra.s	.runner_res_done

.runner_res_apply:
	; Validate the requested rez (must be 0 or 1).
	move.w	RUNNER_REZ, d4
	cmp.w	#1, d4
	bhi.s	.runner_res_bad

	; XBIOS Setscreen(log=-1, phys=-1, rez=d4). -1 = "no change".
	; Stack frame: rez.w + phys.l + log.l + fn.w = 12 bytes total.
	move.w	d4, -(sp)
	move.l	#-1, -(sp)		; phys
	move.l	#-1, -(sp)		; log
	move.w	#XBIOS_Setscreen, -(sp)
	trap	#14
	lea	12(sp), sp

	; Wait for vblank so the rez switch is committed on the next
	; frame before we (or any subsequent command) tries to use the
	; new geometry. Without this Cconws output races the change and
	; lands on a frame that's still being rebuilt by the shifter.
	move.w	#XBIOS_Vsync, -(sp)
	trap	#14
	addq.l	#2, sp

	; Restore a sensible 16-entry palette. Setscreen leaves the
	; colour registers as the previous program (or boot) left them;
	; in particular a program that ran at one rez may have set
	; colour 0 == colour 1, which makes the screen unreadable
	; after we switch to a different bit depth. Med-rez uses 4
	; entries, low-rez uses all 16 — we always load 16 because
	; Setpalette ignores the upper entries in higher-bpp modes.
	pea	default_palette(pc)
	move.w	#XBIOS_Setpalette, -(sp)
	trap	#14
	addq.l	#6, sp

	; Clear the screen and repaint the runner banner in one Cconws —
	; banner_text leads with VT52 ESC E. Without this the screen
	; still has the previous program's pixels at the new rez'
	; geometry, which looks like garbage.
	pea	banner_text(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	moveq	#RUNNER_RES_OK, d3
	bra.s	.runner_res_done

.runner_res_bad:
	moveq	#RUNNER_RES_ERR_BAD, d3

.runner_res_done:
	runner_send_sync RUNNER_CMD_DONE_RES, 4
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
; runner_print_dec_d3 — print signed 32-bit value in d3 as ASCII
; decimal via Cconws. Stack-allocated scratch buffer (12 B). 68000
; only has divu.w (32/16=16q+16r), so a 32-bit division by 10 is
; done as two 16-bit divs: divide the high word, then divide the
; (remainder<<16|low_word) chunk. Both quotients combine to give
; the full 32-bit quotient; the second remainder is the next digit.
; ---------------------------------------------------------------
runner_print_dec_d3:
	movem.l	d0-d2/d4-d5/a0-a1, -(sp)
	move.l	d3, d4			; d4 = working value
	tst.l	d4
	bpl.s	.pd_pos
	; Negative — emit '-' and negate.
	move.w	#'-', -(sp)
	move.w	#GEMDOS_Cconout, -(sp)
	trap	#1
	addq.l	#4, sp
	neg.l	d4
.pd_pos:
	; Stack scratch: 12 bytes, build digits backward.
	lea	-12(sp), sp
	move.l	sp, a0
	lea	11(a0), a1		; one past the buffer end
	clr.b	(a1)
	subq.l	#1, a1			; a1 points at last writable byte

	tst.l	d4
	bne.s	.pd_loop
	; Zero special case.
	move.b	#'0', (a1)
	bra.s	.pd_emit

.pd_loop:
	; d4 / 10 → quotient back into d4, remainder digit (0..9) into d2.
	move.l	d4, d0			; d0 = original 32-bit dividend
	clr.w	d0			; zero the low word
	swap	d0			; d0 = high16 in low word
	divu.w	#10, d0			; d0 high = rem1, d0 low = q1
	move.w	d0, d5			; stash q1
	; Build (rem1 << 16) | low16 in d1 for the second divide.
	swap	d0			; d0 low = rem1
	move.w	d4, d0			; d0 = (rem1 << 16) | low16
	divu.w	#10, d0			; d0 high = rem2 (digit), d0 low = q2
	swap	d5			; d5 high = q1, low = (junk)
	move.w	d0, d5			; d5 low = q2 → d5 = full quotient
	swap	d0			; d0 low = remainder digit
	add.b	#'0', d0
	move.b	d0, (a1)
	subq.l	#1, a1
	move.l	d5, d4
	tst.l	d4
	bne.s	.pd_loop

.pd_emit:
	addq.l	#1, a1			; first written digit
	move.l	a1, -(sp)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	lea	12(sp), sp
	movem.l	(sp)+, d0-d2/d4-d5/a0-a1
	rts

; ---------------------------------------------------------------
; Read-only data (lives in cartridge ROM alongside the code).
; ---------------------------------------------------------------

; banner_text below leads with VT52 ESC E (clear + home cursor) so a
; single Cconws(banner_text) call both wipes the screen and paints
; the banner. Used by runner_entry on first launch and by runner_res
; after a Setscreen+Setpalette so the rez change lands on a clean
; surface.

; STE-style 16-entry palette (4 bits per RGB channel) that also runs
; cleanly on plain ST. The STE shifter uses bit 3 of each nibble as
; the LSB of a 4-bit colour value; the ST shifter ignores it and
; reads only bits 0-2 (3-bit value 0-7). So $0FFF on STE is full
; brightness (15) and on ST it reads as $0777 (max ST brightness, 7)
; — same visual maximum on both, no fork needed. Layout matches TOS
; defaults: col 0 = white (background), col 15 = black (low-rez fg),
; col 3 = black (med-rez fg). runner_res reloads this via XBIOS
; Setpalette after every successful Setscreen.
	cnop	0,2
default_palette:
	dc.w	$0FFF	; 0  white   (bg)
	dc.w	$0F00	; 1  red
	dc.w	$00F0	; 2  green
	dc.w	$0000	; 3  black   (med-rez fg)
	dc.w	$000F	; 4  blue
	dc.w	$0F0F	; 5  magenta
	dc.w	$00FF	; 6  cyan
	dc.w	$0555	; 7  light gray
	dc.w	$0333	; 8  dark gray
	dc.w	$0F33	; 9  light red
	dc.w	$03F3	; 10 light green
	dc.w	$0FF3	; 11 light yellow
	dc.w	$033F	; 12 light blue
	dc.w	$0F3F	; 13 light magenta
	dc.w	$03FF	; 14 light cyan
	dc.w	$0000	; 15 black   (low-rez fg)

; GEMDOS Dsetpath argument — the GEMDRIVE drive root. Backslash
; written as a numeric byte ($5C) so vasm doesn't treat it as a
; string-escape char.
root_path:
	dc.b	$5C, 0
	even

banner_text:
	dc.b	27, "E"			; VT52 clear + home
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
text_exit_open:
	dc.b	13, 10, "[EXIT ", 0
	even
text_exit_close:
	dc.b	"] - ", 0
	even
text_cd:
	dc.b	13, 10, "[CD   ] - ", 0
	even
text_res:
	dc.b	13, 10, "[RES  ]", 13, 10, 0
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
