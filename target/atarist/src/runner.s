; SidecarTridge Multi-device DevOps — Runner module (Epic 03)
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; The cartridge image places this module at offset $1C00 inside the
; 10 KB cartridge code section (RUNNER_BLOB = $FA1C00) via
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
CARTRIDGE_CODE_SIZE		equ $2800			; 10 KB cartridge code budget
SHARED_BLOCK_ADDR		equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)	; $FA2800
CMD_MAGIC_SENTINEL_ADDR		equ SHARED_BLOCK_ADDR		; $FA2800
; APP_BUFFERS starts at SHARED_BLOCK + $100; APP_FREE starts $200
; further in (after the high-res TRANSTABLE used by main.s' boot
; framebuffer code). So APP_FREE = SHARED_BLOCK + $300.
APP_FREE_ADDR			equ (SHARED_BLOCK_ADDR + $300)	; $FA2B00

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
RUNNER_CMD_EXECUTE		equ ($02 + APP_RUNNER)	; Pexec mode 0
RUNNER_CMD_CD			equ ($03 + APP_RUNNER)	; Dsetpath
RUNNER_CMD_RES			equ ($04 + APP_RUNNER)	; XBIOS Setscreen — rez at RUNNER_REZ
RUNNER_CMD_MEMINFO		equ ($05 + APP_RUNNER)	; system memory snapshot (synchronous)
; m68k -> RP report commands (sent via send_sync from the Runner).
RUNNER_CMD_DONE_EXECUTE		equ ($82 + APP_RUNNER)	; payload: i32 exit code
RUNNER_CMD_DONE_CD		equ ($83 + APP_RUNNER)	; payload: i32 GEMDOS errno
RUNNER_CMD_DONE_HELLO		equ ($84 + APP_RUNNER)	; no payload — runner entered loop
RUNNER_CMD_DONE_RES		equ ($85 + APP_RUNNER)	; payload: i32 errno (0 = OK, -1 = mono, -2 = bad rez)
RUNNER_CMD_DONE_MEMINFO		equ ($86 + APP_RUNNER)	; payload: 24-byte meminfo struct

; Constants required by inc/sidecart_macros.s when send_sync expands.
; main.s defines these too; we need our own local copies because
; runner.s is its own assembly unit. Derived from SHARED_BLOCK_ADDR
; so a future cartridge cap bump only touches one constant.
RANDOM_TOKEN_ADDR		equ (SHARED_BLOCK_ADDR + 4)	; $FA2804
RANDOM_TOKEN_SEED_ADDR		equ (SHARED_BLOCK_ADDR + 8)	; $FA2808
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
GEMDOS_Super			equ $20
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
SHARED_VARIABLES_ADDR		equ (SHARED_BLOCK_ADDR + $10)	; $FA2810
SHARED_VAR_GEMDRIVE_RELOC	equ 10
SHARED_VAR_DRIVE_NUMBER		equ 12
SHARED_VAR_ADV_HOOK_VECTOR	equ 16
GEMDRIVE_RELOC_ADDR_VAR		equ (SHARED_VARIABLES_ADDR + SHARED_VAR_GEMDRIVE_RELOC*4)
DRIVE_NUMBER_ADDR		equ (SHARED_VARIABLES_ADDR + SHARED_VAR_DRIVE_NUMBER*4)
ADV_HOOK_VECTOR_VAR		equ (SHARED_VARIABLES_ADDR + SHARED_VAR_ADV_HOOK_VECTOR*4)

; Epic 04 — Advanced Runner. The runner blob now relocates itself
; to RAM at runner_entry so its VBL handler can run from a stable
; RAM address (cartridge ROM is fine for instructions but the VBL
; ISR fires every frame — RAM is safer). Target address is
; gemdrive_reloc - RUNNER_RELOC_OFFSET, leaving $400 of slack
; between the two relocated blobs.
RUNNER_BLOB_CART_ADDR		equ (ROM4_ADDR + $1C00)		; cartridge source
RUNNER_BLOB_SIZE_BYTES		equ $C00			; matches devops.ld slot
RUNNER_RELOC_OFFSET		equ $1000			; runner sits 4 KB below gemdrive
_memtop				equ $436

; Advanced Runner command range. Reuses the existing sentinel; the
; foreground poll loop's cmp.l cascade only matches $05xx, so $06xx
; codes pass through to the Advanced handler.
APP_RUNNER_VBL			equ $0600
RUNNER_ADV_CMD_RESET		equ ($01 + APP_RUNNER_VBL)	; forced cold reset
RUNNER_VBL_RANGE_MASK		equ $FF00

; --- Hook-vector selector (Epic 04 / S3) ---
; The Advanced handler can be installed at one of two vectors.
; Both are simple long pointers and accept the same chain pattern
; (`move.l old(pc), -(sp); rts`), so the handler body is identical;
; only the install address differs.
;
;   $70  — VBL autovector. Fires every vsync (50/60 Hz). Default.
;          Polite to chain, but a program that does
;          `move.l #my_handler, $70.w` without saving the previous
;          value DESTROYS our hook completely — the VBL-driven
;          `adv reset` cannot recover such a program.
;   $400 — etv_timer ETV vector. Fires every 200 Hz from TOS' MFP
;          timer-C handler. Higher rate (5 ms) but our handler is
;          minimal so the overhead is negligible. Much harder for
;          a wedged program to break — TOS' keyboard, mouse and
;          sound subsystems all chain through this vector, so a
;          wholesale replacement breaks the program itself before
;          it can wedge anything else.
;
; Toggle by changing ADV_HOOK_VECTOR below. No other code change
; needed — install / chain / uninstall all use this constant.
ADV_HOOK_VECTOR_VBL		equ $70
ADV_HOOK_VECTOR_ETV_TIMER	equ $400
ADV_HOOK_VECTOR			equ ADV_HOOK_VECTOR_ETV_TIMER

; runner.s must be 100% relocatable and self-contained — no xref /
; xdef cross-module references, no jsr / jmp to symbols outside
; this assembly unit. The protocol macros (send_sync,
; send_write_sync) live in inc/sidecart_macros.s and bsr.w into
; functions defined in inc/sidecart_functions.s; both files are
; included verbatim below so runner.o gets its own private copies
; of those functions and the bsr's resolve locally. vasm doesn't
; export plain labels, so vlink doesn't see duplicates against
; main.o or gemdrive.o's copies. Same pattern gemdrive.s uses.
	include	"inc/sidecart_macros.s"
	include	"inc/tos.s"

; ---------------------------------------------------------------
; Entry point — offset 0 of the runner blob. Reached via
; `jmp RUNNER_BLOB` from main.s's check_commands dispatch.
; ---------------------------------------------------------------

runner_entry:
	; --- Stage 0: relocation harness — runs ONCE from cartridge
	; ROM at $FA1C00. Copies the entire runner blob to RAM
	; (gemdrive_reloc - RUNNER_RELOC_OFFSET), lowers _memtop to
	; protect both blobs, then jmps into the relocated copy at
	; runner_post_reloc. The cartridge-ROM source is unused after
	; the copy; the RAM copy is what actually executes for the
	; lifetime of this Runner session. The whole module is
	; PC-relative so labels resolve correctly post-relocation. ---
	move.l	GEMDRIVE_RELOC_ADDR_VAR, d0
	sub.l	#RUNNER_RELOC_OFFSET, d0	; d0 = runner_reloc
	move.l	d0, _memtop.w			; lower _memtop to protect us

	move.l	d0, a1				; dest
	move.l	#RUNNER_BLOB_CART_ADDR, a0	; source
	move.l	#RUNNER_BLOB_SIZE_BYTES, d1
	lsr.l	#2, d1
	subq.l	#1, d1
.runner_copy:
	move.l	(a0)+, (a1)+
	dbf	d1, .runner_copy

	; Compute relocated runner_post_reloc and jump.
	move.l	d0, a0
	add.l	#(runner_post_reloc - runner_entry), a0
	jmp	(a0)

; --- Stage 1+: post-relocation. Everything below runs from the
; relocated RAM copy. ---
runner_post_reloc:
	; The cartridge address space ($FA0000-$FAFFFF) is read-only
	; from the m68k — the cartridge port has no write strobe and
	; any store there bus-errors. The Runner therefore never writes
	; to the shared region directly; cross-target state lives in
	; APP_FREE (RP writes), shared variables (RP writes via
	; chandler), or the RP's in-memory mirror (set by cmdRunner /
	; the chandler callbacks).

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

	; --- Step 0.5 (Epic 04 / S1+S3+S4): install the Advanced Runner
	; hook at the vector chosen by the RP-side aconfig setting. The
	; resolved vector address ($70 for VBL or $400 for etv_timer) is
	; published to shared-var slot 16 by gemdrive_command_cb's HELLO
	; handler before the runner is launched. We read it indirectly
	; into a2; if the slot is zero (older RP firmware that doesn't
	; publish it) we fall back to the build-time ADV_HOOK_VECTOR
	; macro. We're already in supervisor mode (inherited from
	; CA_INIT bit 27), so direct vector write is fine. Mask
	; interrupts during the swap so we never run a partially-updated
	; vector. ---
	move.l	ADV_HOOK_VECTOR_VAR, d2
	tst.l	d2
	bne.s	.adv_have_addr
	move.l	#ADV_HOOK_VECTOR, d2		; fallback to build-time choice
.adv_have_addr:
	move.l	d2, a2				; a2 = vector address (1024 max → indirect ok)
	; Stash the resolved hook-vector address inside the relocated
	; blob so the HELLO payload below can echo a vector ID back to
	; the RP without re-reading the shared var.
	lea	adv_active_vec(pc), a1
	move.l	d2, (a1)

	move.w	sr, -(sp)
	or.w	#$0700, sr
	lea	old_adv_hook(pc), a1		; PC-rel into the relocated blob
	move.l	(a2), (a1)			; (a1) = old handler address
	lea	adv_hook_handler(pc), a0	; relocated handler address
	move.l	a0, (a2)			; install at the chosen vector
	move.w	(sp)+, sr

	; Tell the RP a fresh Runner session just started so it clears
	; any stale busy lock / cwd mirror that survived a physical or
	; cold reset. d3 layout:
	;   bit 0      : advanced_installed (always 1 here)
	;   bits 8..15 : hook_vector_id (0 = vbl @ $70,
	;                                 1 = etv_timer @ $400,
	;                                 0xFF = unknown — shouldn't happen)
	moveq.l	#0, d3
	move.b	#1, d3				; bit 0 = advanced installed
	move.l	adv_active_vec(pc), d4
	cmpi.l	#$70, d4
	bne.s	.adv_hello_etv
	; vector ID 0 — already in the high byte of d3 (still 0).
	bra.s	.adv_hello_send
.adv_hello_etv:
	cmpi.l	#$400, d4
	bne.s	.adv_hello_unknown
	ori.l	#$0100, d3			; bits 8..15 = 1 (etv_timer)
	bra.s	.adv_hello_send
.adv_hello_unknown:
	ori.l	#$FF00, d3			; bits 8..15 = 0xFF (unknown)
.adv_hello_send:
	send_sync RUNNER_CMD_DONE_HELLO, 4

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
	; Cold reset is handled by the Advanced Runner VBL hook
	; (RUNNER_ADV_CMD_RESET in the $06xx range — see adv_hook_handler
	; below). The foreground RUNNER_CMD_RESET dispatch was retired
	; in Epic 04 / S5 because the VBL path also escapes wedged
	; programs that the foreground poll can't reach.
	move.l	CMD_MAGIC_SENTINEL_ADDR, d6
	cmp.l	#RUNNER_CMD_EXECUTE, d6
	beq	runner_execute
	cmp.l	#RUNNER_CMD_CD, d6
	beq	runner_cd
	cmp.l	#RUNNER_CMD_RES, d6
	beq	runner_res
	cmp.l	#RUNNER_CMD_MEMINFO, d6
	beq	runner_meminfo
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
	send_sync RUNNER_CMD_DONE_EXECUTE, 4
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
	send_sync RUNNER_CMD_DONE_CD, 4
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
	send_sync RUNNER_CMD_DONE_RES, 4
	bra	runner_poll_loop

; RUNNER_CMD_MEMINFO handler. Reads system memory cookies and the
; MMU bank-config register, packs a 24-byte struct on the stack and
; ships it back to the RP via send_write_sync. Struct layout (the
; m68k writes the values big-endian as the CPU sees them; the RP-side
; chandler iterates 16-bit words and unswaps as needed):
;
;   offset  size  field
;     0     u32   _membot   ($432)
;     4     u32   _memtop   ($436)
;     8     u32   _phystop  ($42E)
;    12     u32   screenmem ($44E)
;    16     u32   basepage  ($4F2 — TOS >= 1.04; 0 on older TOS)
;    20     u16   bank0_kb  (decoded from $FFFF8001 lower nibble)
;    22     u16   bank1_kb
;
; $FFFF8001 lives in supervisor address space, so the handler enters
; supervisor via GEMDOS Super(NULL), reads the byte, then restores
; user mode via Super(<old_ssp>). All other addresses are in the
; system-variable RAM area — readable from user mode.
;
; Bank decoding (bits 3..0 of $FFFF8001):
;   0000 -> 128  / 128
;   0100 -> 512  / 128
;   0101 -> 512  / 512
;   1000 -> 2048 / 128
;   1010 -> 2048 / 2048
;   any other -> bank0_kb = bank1_kb = 0  (caller flags as unknown)
runner_meminfo:
	; Trace.
	pea	text_mem(pc)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; Allocate 24-byte struct on the stack and zero it. We're already
	; in supervisor mode (the runner's whole call chain inherits the
	; supervisor state from CA_INIT bit 27 / pre_auto), so no Super
	; toggle is needed for $0..$7FF or $FFFF8001 reads.
	;
	; STAGED IMPLEMENTATION: only _membot ($432) is read for now to
	; validate the wire/buffer path end-to-end. All other fields
	; ship as zero. Once meminfo returns the correct $432 value,
	; subsequent fields will be filled in one at a time.
	lea	-24(sp), sp
	move.l	sp, a4
	; Zero the struct.
	clr.l	0(a4)
	clr.l	4(a4)
	clr.l	8(a4)
	clr.l	12(a4)
	clr.l	16(a4)
	clr.l	20(a4)

	; Stage 4: full struct. _membot ($432), _memtop ($436),
	; _phystop ($42E), screenmem ($44E), basepage ($4F2), and the
	; MMU bank-config byte at $FFFF8001.
	move.l	$432.w, 0(a4)			; _membot
	move.l	$436.w, 4(a4)			; _memtop
	move.l	$42E.w, 8(a4)			; _phystop
	move.l	$44E.w, 12(a4)			; screenmem (logical screen base)
	move.l	$4F2.w, 16(a4)			; _run (basepage; 0 on TOS < 1.04)

	; Read the MMU configuration register at $FFFF8001. The byte's
	; lower nibble encodes the (bank0, bank1) sizes. We're already
	; in supervisor mode (inherited from CA_INIT bit 27), so the
	; absolute address read is allowed. The buffer's bank fields at
	; offsets 20 and 22 were pre-zeroed, so any unrecognised nibble
	; falls through with bank0_kb = bank1_kb = 0 — the RP-side
	; reports that as "unrecognised MMU config".
	moveq.l	#0, d3
	move.b	$FFFF8001, d3
	and.w	#$0F, d3

	cmp.b	#%0000, d3
	bne.s	.mi_n0100
	move.w	#128, 20(a4)
	move.w	#128, 22(a4)
	bra.s	.mi_decoded
.mi_n0100:
	cmp.b	#%0100, d3
	bne.s	.mi_n0101
	move.w	#512, 20(a4)
	move.w	#128, 22(a4)
	bra.s	.mi_decoded
.mi_n0101:
	cmp.b	#%0101, d3
	bne.s	.mi_n1000
	move.w	#512, 20(a4)
	move.w	#512, 22(a4)
	bra.s	.mi_decoded
.mi_n1000:
	cmp.b	#%1000, d3
	bne.s	.mi_n1010
	move.w	#2048, 20(a4)
	move.w	#128, 22(a4)
	bra.s	.mi_decoded
.mi_n1010:
	cmp.b	#%1010, d3
	bne.s	.mi_decoded
	move.w	#2048, 20(a4)
	move.w	#2048, 22(a4)
.mi_decoded:

	; Ship the struct to the RP. send_write_sync expects a4 = buffer.
	send_write_sync RUNNER_CMD_DONE_MEMINFO, 24

	lea	24(sp), sp			; pop struct
	bra	runner_poll_loop

; ---------------------------------------------------------------
; Advanced Runner hook (Epic 04 / S1+S2+S3).
;
; Installed at ADV_HOOK_VECTOR ($70 VBL or $400 etv_timer) by
; runner_post_reloc. Same handler body services both — the chain
; pattern (`move.l old(pc), -(sp); rts`) works for an autovectored
; interrupt (rts → chained handler → rte) and for an ETV-style
; subroutine (rts → chained handler → rts back to TOS' MFP ISR
; which then rte's). ETV convention says d0/d1/a0/a1 are scratch,
; which our save list (d0/d1/a0) is a strict subset of.
;
; Read the cartridge sentinel and check whether the upper byte of
; the command code matches APP_RUNNER_VBL ($0600). If so, dispatch.
; Then fall through to the saved handler regardless.
;
; The sentinel is written by the RP via SEND_COMMAND_TO_DISPLAY;
; values in the $05xx range belong to the foreground poll loop,
; values in the $06xx range belong to us. The two ranges never
; collide so the poll loop's existing cmp.l cascade ignores us
; and we ignore everything outside $06xx.
; ---------------------------------------------------------------
adv_hook_handler:
	movem.l	d0-d1/a0, -(sp)			; ISR prologue — minimal save
	move.l	CMD_MAGIC_SENTINEL_ADDR, d0
	move.l	d0, d1
	andi.l	#$FFFFFF00, d1
	cmpi.l	#APP_RUNNER_VBL, d1
	bne	.adv_chain
	; In our range. Compare against each known command code.
	cmpi.l	#RUNNER_ADV_CMD_RESET, d0
	beq	adv_force_reset
	; Unknown $06xx command — ignore and chain. (Logging on the
	; m68k from inside an ISR is a hazard; the RP-side handler
	; already validated the command code before firing the sentinel.)
.adv_chain:
	movem.l	(sp)+, d0-d1/a0
	move.l	old_adv_hook(pc), -(sp)
	rts					; chain → previous handler

; --- Forced cold reset (Epic 04 / S2). Mirrors the foreground
; runner_reset's memvalid clear + jmp through the reset vector at
; $4.w, but driven from inside the VBL ISR so it works even when
; the foreground poll loop is wedged (program in an infinite loop,
; bombs already painted, etc.). No register restore — the cold
; reset wipes everything. No rte — control transfers to TOS' init
; permanently. SR's IPL is reset by TOS' init early in its
; sequence so the in-interrupt entry doesn't matter. ---
adv_force_reset:
	clr.l	$420.w			; memvalid
	clr.l	$43A.w			; memval2
	clr.l	$51A.w			; memval3
	move.l	$4.w, a0		; reset vector
	jmp	(a0)
	; unreachable

; old_adv_hook: 4-byte cell holding the address of the prior
; vector (saved at install time by runner_post_reloc — could be a
; previous VBL handler or a previous etv_timer ETV depending on
; ADV_HOOK_VECTOR). adv_active_vec mirrors the vector address that
; was actually installed (from shared-var slot 16, or the build-time
; fallback) so the HELLO payload can echo a vector ID back to the
; RP without re-reading the shared var. PC-relative reads work at
; runtime once the blob is relocated; both cells live in the
; relocated copy, NOT the cartridge ROM original.
	cnop	0,4
	dc.l	'XBRA'				; XBRA debug marker so chain
	dc.l	'SDRA'				; walkers can identify our hook
old_adv_hook:
	dc.l	0
adv_active_vec:
	dc.l	0
	even

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
; Private copy of the shared sidecart protocol functions
; (send_sync_command_to_sidecart + send_sync_write_command_to_sidecart),
; included so the bsr's emitted by send_sync / send_write_sync from
; inc/sidecart_macros.s resolve inside this assembly unit. Same
; pattern as gemdrive.s — vasm doesn't export plain labels so this
; doesn't conflict with main.o's or gemdrive.o's copies.
; ---------------------------------------------------------------
	include	"inc/sidecart_functions.s"

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
text_mem:
	dc.b	13, 10, "[MEMIN] - Reading memory information", 13, 10, 0
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
