; SidecarTridge Multi-device GEMDRIVE — Atari ST module
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; Cartridge image places this module at offset $0800 (GEMDRIVE_BLOB =
; $FA0800) via target/atarist/src/devops.ld; main.s reaches it through
; `rom_function: jmp GEMDRIVE_BLOB+4` for the diagnostic entry, and via
; `jsr (relocated_addr)` from gemdrive_init for the install entry.
;
; This blob is copied byte-for-byte to a runtime-chosen RAM address by
; main.s's gemdrive_init. Therefore: use only PC-relative addressing
; for any reference inside this file (lea label(pc), bra, bsr, etc.).
;
; The shared protocol functions (send_sync_command_to_sidecart and
; send_sync_write_command_to_sidecart) are pulled in via
; `include "inc/sidecart_functions.s"` AT THE END of this file. That
; gives gemdrive.o its own private copy of the functions, located inside
; the relocated blob — they execute from RAM rather than cartridge ROM
; once the copy lands. The same pattern is used by every per-module .s
; file in md-drives-emulator (gemdrive.s / floppy.s / rtc.s / acsi.s).
; vasm doesn't export plain labels, so vlink doesn't see a duplicate
; with main.o's copy.

	section text

; --- Constants required by inc/sidecart_functions.s when it lands at
; the end of this file. Same values as main.s; defined here so the
; include can compile cleanly inside this assembly unit. ---
_dskbufp		equ $4C6
RANDOM_TOKEN_ADDR	equ $FA2004
RANDOM_TOKEN_SEED_ADDR	equ $FA2008
RANDOM_TOKEN_POST_WAIT	equ $1
ROMCMD_START_ADDR	equ $FB0000
CMD_MAGIC_NUMBER	equ $ABCD
CMD_RETRIES_COUNT	equ 3
CMD_SET_SHARED_VAR	equ 1
COMMAND_TIMEOUT		equ $0000FFFF
COMMAND_WRITE_TIMEOUT	equ COMMAND_TIMEOUT

; --- Macros + GEMDOS sysvar / function constants. ---
	include	"inc/sidecart_macros.s"
	include	"inc/tos.s"

; --- TOS / XBIOS aliases used directly by this file ---
GEMDOS_Cconws		equ 9
GEMDOS_Dgetdrv		equ $19
VEC_GEMDOS		equ $21			; trap #1 vector slot in exception table
XBIOS_Setexc		equ 5
_drvbits		equ $4C2
_bootdev		equ $446
_nflops			equ $4A6
_longframe		equ $59E

; --- GEMDRIVE wire format (kept identical to md-drives-emulator) ---
APP_GEMDRVEMUL		equ $0400
CMD_RESET_GEM		equ ($0 + APP_GEMDRVEMUL)
CMD_SAVE_VECTORS	equ ($1 + APP_GEMDRVEMUL)
CMD_REENTRY_LOCK	equ ($3 + APP_GEMDRVEMUL)
CMD_REENTRY_UNLOCK	equ ($4 + APP_GEMDRVEMUL)
CMD_FSETDTA_CALL	equ ($1A + APP_GEMDRVEMUL)
CMD_DFREE_CALL		equ ($36 + APP_GEMDRVEMUL)
CMD_DCREATE_CALL	equ ($39 + APP_GEMDRVEMUL)
CMD_DDELETE_CALL	equ ($3A + APP_GEMDRVEMUL)
CMD_DSETPATH_CALL	equ ($3B + APP_GEMDRVEMUL)
CMD_FCREATE_CALL	equ ($3C + APP_GEMDRVEMUL)
CMD_FOPEN_CALL		equ ($3D + APP_GEMDRVEMUL)
CMD_FCLOSE_CALL		equ ($3E + APP_GEMDRVEMUL)
CMD_FDELETE_CALL	equ ($41 + APP_GEMDRVEMUL)
CMD_FSEEK_CALL		equ ($42 + APP_GEMDRVEMUL)
CMD_FATTRIB_CALL	equ ($43 + APP_GEMDRVEMUL)
CMD_DGETPATH_CALL	equ ($47 + APP_GEMDRVEMUL)
CMD_PEXEC_CALL		equ ($4B + APP_GEMDRVEMUL)
CMD_FSFIRST_CALL	equ ($4E + APP_GEMDRVEMUL)
CMD_FSNEXT_CALL		equ ($4F + APP_GEMDRVEMUL)
CMD_GEMDRIVE_VERIFY_MEMTOP	equ ($52 + APP_GEMDRVEMUL)
CMD_FRENAME_CALL	equ ($56 + APP_GEMDRVEMUL)
CMD_FDATETIME_CALL	equ ($57 + APP_GEMDRVEMUL)
CMD_READ_BUFF_CALL	equ ($81 + APP_GEMDRVEMUL)
CMD_SAVE_BASEPAGE	equ ($83 + APP_GEMDRVEMUL)
CMD_SAVE_EXEC_HEADER	equ ($84 + APP_GEMDRVEMUL)
CMD_WRITE_BUFF_CALL	equ ($88 + APP_GEMDRVEMUL)
CMD_DTA_EXIST_CALL	equ ($8A + APP_GEMDRVEMUL)
CMD_DTA_RELEASE_CALL	equ ($8B + APP_GEMDRVEMUL)

; --- GEMDOS function codes used internally ---
GEMDOS_Dsetdrv		equ $E
GEMDOS_Fgetdta		equ $2F
GEMDOS_Pexec		equ $4B
GEMDOS_Mfree		equ $49

; --- Pexec mode codes ---
PE_LOAD_GO		equ 0
PE_LOAD			equ 3
PE_GO			equ 4
PE_CREATE_BASEPAGE	equ 5
PE_GO_AND_FREE		equ 6

PRG_STRUCT_SIZE		equ 28
PRG_MAGIC_NUMBER	equ $601A

; --- GEMDOS error codes ---
GEMDOS_EIO_READ		equ -93
GEMDOS_EIO_WRITE	equ -92

; --- DTA layout ---
DTA_SIZE		equ 44
BUFFER_READ_SIZE	equ 4096

; --- Shared region addresses (must match rp/src/include/gemdrive.h) ---
ROM4_ADDR			equ $FA0000
SHARED_VARIABLES		equ (ROM4_ADDR + $2010)
APP_FREE_ADDR			equ (ROM4_ADDR + $2300)

SHARED_VAR_DRIVE_NUMBER		equ 12
SHARED_VAR_DRIVE_LETTER		equ 13
SHARED_VAR_REENTRY_TRAP		equ 14
SHARED_VAR_FIRST_FILE_DESCRIPTOR equ 15

GEMDRIVE_DEFAULT_PATH		equ APP_FREE_ADDR
GEMDRIVE_DEFAULT_PATH_LEN	equ 128
GEMDRIVE_DFREE_STATUS		equ (GEMDRIVE_DEFAULT_PATH + GEMDRIVE_DEFAULT_PATH_LEN)
GEMDRIVE_DFREE_STRUCT		equ (GEMDRIVE_DFREE_STATUS + 4)

; --- Phase 2 state region (offsets must match rp/src/include/gemdrive.h) ---
GEMDRIVE_SET_DPATH_STATUS	equ (APP_FREE_ADDR + $090)
GEMDRIVE_FOPEN_HANDLE		equ (APP_FREE_ADDR + $094)
GEMDRIVE_FCLOSE_STATUS		equ (APP_FREE_ADDR + $098)
GEMDRIVE_FSEEK_STATUS		equ (APP_FREE_ADDR + $09C)
GEMDRIVE_READ_BYTES		equ (APP_FREE_ADDR + $0A0)
GEMDRIVE_READ_BUFFER		equ (APP_FREE_ADDR + $0A4)
GEMDRIVE_DTA_F_FOUND		equ (APP_FREE_ADDR + $10A4)
GEMDRIVE_DTA_TRANSFER		equ (APP_FREE_ADDR + $10A8)
; --- S4 write-side state ---
GEMDRIVE_DCREATE_STATUS		equ (APP_FREE_ADDR + $10D8)
GEMDRIVE_DDELETE_STATUS		equ (APP_FREE_ADDR + $10DC)
GEMDRIVE_FCREATE_HANDLE		equ (APP_FREE_ADDR + $10E0)
GEMDRIVE_FDELETE_STATUS		equ (APP_FREE_ADDR + $10E4)
GEMDRIVE_FATTRIB_STATUS		equ (APP_FREE_ADDR + $10E8)
GEMDRIVE_FRENAME_STATUS		equ (APP_FREE_ADDR + $10EC)
GEMDRIVE_FDATETIME_DATE		equ (APP_FREE_ADDR + $1620)
GEMDRIVE_FDATETIME_TIME		equ (APP_FREE_ADDR + $1624)
GEMDRIVE_FDATETIME_STATUS	equ (APP_FREE_ADDR + $1628)
GEMDRIVE_WRITE_BYTES		equ (APP_FREE_ADDR + $10F0)
BUFFER_WRITE_SIZE		equ 1024

; --- S5 Pexec / DTA state ---
GEMDRIVE_PEXEC_MODE		equ (APP_FREE_ADDR + $14F4)
GEMDRIVE_PEXEC_STACK_ADDR	equ (APP_FREE_ADDR + $14F8)
GEMDRIVE_PEXEC_FNAME		equ (APP_FREE_ADDR + $14FC)
GEMDRIVE_PEXEC_CMDLINE		equ (APP_FREE_ADDR + $1500)
GEMDRIVE_PEXEC_ENVSTR		equ (APP_FREE_ADDR + $1504)
; EXEC_HEADER (28 B) is placed BEFORE EXEC_PD (256 B basepage) so the
; basepage's memcpy never overlaps the header — see RP-side comment.
GEMDRIVE_EXEC_HEADER		equ (APP_FREE_ADDR + $150C)
GEMDRIVE_EXEC_PD		equ (APP_FREE_ADDR + $1534)

; ====================================================================
; MACROS
; ====================================================================

; Save scratch registers used by GEMDOS handlers. a0/a1 are NOT saved:
; a0 stays as the args-frame pointer set up by exec_trapped_handler, a1
; is scratch for the dispatch table lookup. d0 is the return value.
save_regs	macro
		movem.l	d1-d7/a2-a6, -(sp)
		endm

restore_regs	macro
		movem.l	(sp)+, d1-d7/a2-a6
		endm

; send_sync / send_write_sync come from inc/sidecart_macros.s included
; near the top. Their bsr resolves PC-relatively to gemdrive.o's local
; copy of send_sync*_command_to_sidecart (pulled in via the
; sidecart_functions.s include at the bottom of this file), so the
; entire request path runs from the relocated blob in RAM — never from
; cartridge-ROM space.

; Restore registers and rte through the original trap exception frame.
return_rte	macro
		restore_regs
		rte
		endm

; Return a word/long status from the shared region as a sign-extended
; longword in d0, then return_rte.
return_interrupt_w	macro
		move.w	\1, d0
		ext.l	d0
		return_rte
		endm

return_interrupt_l	macro
		move.l	\1, d0
		return_rte
		endm

; Lock GEMDOS reentry — the RP sets the SHARED_VAR_REENTRY_TRAP slot,
; which gemdrive_trap polls to short-circuit nested trap #1 calls
; (e.g., a handler internally calling Dgetdrv) straight to old_handler.
reentry_gem_lock	macro
		send_sync CMD_REENTRY_LOCK, 0
		endm

reentry_gem_unlock	macro
		send_sync CMD_REENTRY_UNLOCK, 0
		endm

; If the current GEMDOS drive is not the emulated one, branch to
; .exec_old_handler. Calls Dgetdrv internally with reentry locked.
detect_emulated_drive	macro
		reentry_gem_lock
		move.w	#GEMDOS_Dgetdrv, -(sp)
		trap	#1
		addq.l	#2, sp
		move.l	d0, -(sp)
		reentry_gem_unlock
		move.l	(sp)+, d0
		and.l	#$FFFF, d0
		cmp.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d0
		bne	.exec_old_handler
		endm

; If a4 points to a path that starts with "X:" check that 'X' matches
; the emulated drive letter; otherwise fall through to the current-drive
; check via detect_emulated_drive. Branches to .exec_old_handler when
; the path is not on the emulated drive.
detect_emulated_drive_letter	macro
		cmp.b	#':', 1(a4)
		bne.s	.\@check_current
		move.b	(a4), d0
		cmp.b	(SHARED_VARIABLES+3+(SHARED_VAR_DRIVE_LETTER*4)), d0
		bne	.exec_old_handler
		bra.s	.\@done
.\@check_current:
		detect_emulated_drive
.\@done:
		endm

; If d3 holds a file handle that's NOT in our emulated range
; (handle < FIRST_FILE_DESCRIPTOR), branch to .exec_old_handler. The
; FIRST_FILE_DESCRIPTOR slot stores the lower bound; the m68k reads it
; as a word (it never exceeds 0x7FFF).
detect_emulated_file_handler	macro
		cmp.w	(SHARED_VARIABLES+2+(SHARED_VAR_FIRST_FILE_DESCRIPTOR*4)), d3
		blt	.exec_old_handler
		endm

; ====================================================================
; ENTRY TABLE — first 8 bytes of the relocated blob.
; offset 0: install_entry  (called from main.s gemdrive_init after copy)
; offset 4: diagnostic_entry (called from rom_function on [F]irmware)
; ====================================================================
	bra.w	install_entry			; offset 0
	bra.w	diagnostic_entry		; offset 4

; ====================================================================
; install_entry — installs gemdrive_trap as trap #1, advertises drive
; ====================================================================
install_entry:
	movem.l	d0-d7/a0-a6, -(sp)

	; XBIOS Setexc(VEC_GEMDOS, &gemdrive_trap). PC-relative lea gives
	; the runtime address; that's the value we want in the vector.
	lea	gemdrive_trap(pc), a0
	move.l	a0, -(sp)
	move.w	#VEC_GEMDOS, -(sp)
	move.w	#XBIOS_Setexc, -(sp)
	trap	#13
	addq.l	#8, sp
	; d0.l = previous GEMDOS vector

	; Stash the old vector in our local cell.
	lea	old_handler(pc), a1
	move.l	d0, (a1)

	; Notify RP — d3 = old vec, d4 = address of our cell.
	move.l	d0, d3
	move.l	a1, d4
	send_sync CMD_SAVE_VECTORS, 8

	; Advertise the emulated drive in TOS sysvars. The RP-side gemdrive
	; HELLO callback already published the drive number into shared
	; var slot 12; we OR its bit into _drvbits and (if drive C) set
	; _bootdev. If there are no real floppies, simulate floppy A so
	; TOS keeps booting through its disk-boot logic.
	tst.w	_nflops.w
	bne.s	.has_floppy
	move.l	#1, _drvbits.w
	move.w	#1, _nflops.w
.has_floppy:
	move.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d0
	moveq.l	#1, d1
	lsl.l	d0, d1
	or.l	d1, _drvbits.w
	cmp.w	#2, d0
	bne.s	.skip_bootdev
	move.w	d0, _bootdev.w
.skip_bootdev:

	movem.l	(sp)+, d0-d7/a0-a6
	rts

; ====================================================================
; diagnostic_entry — reachable via [F]irmware terminal command
; ====================================================================
diagnostic_entry:
	movem.l	d0-d7/a0-a3, -(sp)

	lea	hello_msg(pc), a0
	move.l	a0, -(sp)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	; Verify-memtop probe — fires AFTER any TOS-side handling has had
	; a chance to disturb the patch.
	move.l	memtop.w, d3
	send_sync CMD_GEMDRIVE_VERIFY_MEMTOP, 4

	movem.l	(sp)+, d0-d7/a0-a3
	rts

hello_msg:
	dc.b	27,"E"
	dc.b	"GEMDRIVE active.",13,10
	dc.b	"  - Read/write file ops",13,10
	dc.b	"  - Folder create/delete/rename",13,10
	dc.b	"  - Pexec for PRG launch",13,10
	dc.b	0
	even

; ====================================================================
; old_handler cell — populated by install_entry via PC-relative write.
; XBRA structure makes the trap chain visible to tools that walk it.
; ====================================================================
	cnop	0,4
	dc.l	'XBRA'
	dc.l	'SDGD'
old_handler:
	dc.l	0
	even

; ====================================================================
; gemdrive_trap — full trap #1 dispatcher
;
; The CPU has already pushed the trap exception frame ([SR, PC]) on
; the supervisor stack. Flow:
;   1. If reentry is locked (RP set the flag during a handler that
;      calls back into GEMDOS), just chain to old_handler — same
;      behaviour as S2 pass-through.
;   2. Otherwise compute a0 = address of the args frame (different
;      stack for user vs supervisor mode, +2 for long-frame CPUs).
;   3. save_regs (d1-d7/a2-a6 — preserves a0/a1/d0).
;   4. Read the GEMDOS opcode at offset 6(a0) and indexed-jmp into
;      the dispatch table; the table entry is a bra.w that branches
;      to the actual handler.
; ====================================================================
gemdrive_trap:
	; Reentry shortcut: GEMDOS calls invoked from inside our own
	; handlers (which may need Dgetdrv, Fgetdta, etc.) bypass the
	; full dispatch and chain straight to the original handler.
	tst.l	(SHARED_VARIABLES+(SHARED_VAR_REENTRY_TRAP*4))
	beq.s	.no_reentry
	move.l	old_handler(pc), -(sp)
	rts
.no_reentry:
	; Determine which stack the caller's args are on.
	btst	#5, (sp)			; S-bit of saved SR
	beq.s	.from_user
.from_super:
	move.l	sp, a0				; supervisor: args follow [SR,PC] on SSP
	bra.s	.check_cpu
.from_user:
	move.l	usp, a0				; user: args on USP
	subq.l	#6, a0				; offset so 6(a0) = func code
.check_cpu:
	tst.w	_longframe.w
	beq.s	.not_long
	addq.w	#2, a0				; long-frame trap pushes +2 bytes
.not_long:
	save_regs

	; Look up the handler. Each table entry is a 4-byte bra.w to the
	; handler; opcode * 4 = byte offset within the table.
	move.w	6(a0), d3
	and.l	#$FFFF, d3
	cmp.w	#$57, d3
	bhi	.exec_old_handler
	add.w	d3, d3
	add.w	d3, d3
	lea	.gemdos_dispatch_table(pc), a1
	jmp	0(a1, d3.w)

.exec_old_handler:
	restore_regs
	move.l	old_handler(pc), -(sp)
	rts

	even
.gemdos_dispatch_table:
	bra.w	.exec_old_handler		; 0x00
	bra.w	.exec_old_handler		; 0x01
	bra.w	.exec_old_handler		; 0x02
	bra.w	.exec_old_handler		; 0x03
	bra.w	.exec_old_handler		; 0x04
	bra.w	.exec_old_handler		; 0x05
	bra.w	.exec_old_handler		; 0x06
	bra.w	.exec_old_handler		; 0x07
	bra.w	.exec_old_handler		; 0x08
	bra.w	.exec_old_handler		; 0x09
	bra.w	.exec_old_handler		; 0x0A
	bra.w	.exec_old_handler		; 0x0B
	bra.w	.exec_old_handler		; 0x0C
	bra.w	.exec_old_handler		; 0x0D
	bra.w	.exec_old_handler		; 0x0E
	bra.w	.exec_old_handler		; 0x0F
	bra.w	.exec_old_handler		; 0x10
	bra.w	.exec_old_handler		; 0x11
	bra.w	.exec_old_handler		; 0x12
	bra.w	.exec_old_handler		; 0x13
	bra.w	.exec_old_handler		; 0x14
	bra.w	.exec_old_handler		; 0x15
	bra.w	.exec_old_handler		; 0x16
	bra.w	.exec_old_handler		; 0x17
	bra.w	.exec_old_handler		; 0x18
	bra.w	.exec_old_handler		; 0x19 Dgetdrv (no intercept)
	bra.w	.Fsetdta			; 0x1A Fsetdta
	bra.w	.exec_old_handler		; 0x1B
	bra.w	.exec_old_handler		; 0x1C
	bra.w	.exec_old_handler		; 0x1D
	bra.w	.exec_old_handler		; 0x1E
	bra.w	.exec_old_handler		; 0x1F
	bra.w	.exec_old_handler		; 0x20
	bra.w	.exec_old_handler		; 0x21
	bra.w	.exec_old_handler		; 0x22
	bra.w	.exec_old_handler		; 0x23
	bra.w	.exec_old_handler		; 0x24
	bra.w	.exec_old_handler		; 0x25
	bra.w	.exec_old_handler		; 0x26
	bra.w	.exec_old_handler		; 0x27
	bra.w	.exec_old_handler		; 0x28
	bra.w	.exec_old_handler		; 0x29
	bra.w	.exec_old_handler		; 0x2A
	bra.w	.exec_old_handler		; 0x2B
	bra.w	.exec_old_handler		; 0x2C
	bra.w	.exec_old_handler		; 0x2D
	bra.w	.exec_old_handler		; 0x2E
	bra.w	.exec_old_handler		; 0x2F
	bra.w	.exec_old_handler		; 0x30
	bra.w	.exec_old_handler		; 0x31
	bra.w	.exec_old_handler		; 0x32
	bra.w	.exec_old_handler		; 0x33
	bra.w	.exec_old_handler		; 0x34
	bra.w	.exec_old_handler		; 0x35
	bra.w	.Dfree				; 0x36 Dfree
	bra.w	.exec_old_handler		; 0x37
	bra.w	.exec_old_handler		; 0x38
	bra.w	.Dcreate			; 0x39 Dcreate
	bra.w	.Ddelete			; 0x3A Ddelete
	bra.w	.Dsetpath			; 0x3B Dsetpath
	bra.w	.Fcreate			; 0x3C Fcreate
	bra.w	.Fopen				; 0x3D Fopen
	bra.w	.Fclose				; 0x3E Fclose
	bra.w	.Fread				; 0x3F Fread
	bra.w	.Fwrite				; 0x40 Fwrite
	bra.w	.Fdelete			; 0x41 Fdelete
	bra.w	.Fseek				; 0x42 Fseek
	bra.w	.Fattrib			; 0x43 Fattrib
	bra.w	.exec_old_handler		; 0x44
	bra.w	.exec_old_handler		; 0x45
	bra.w	.exec_old_handler		; 0x46
	bra.w	.Dgetpath			; 0x47 Dgetpath
	bra.w	.exec_old_handler		; 0x48
	bra.w	.exec_old_handler		; 0x49
	bra.w	.exec_old_handler		; 0x4A
	bra.w	.Pexec				; 0x4B Pexec
	bra.w	.exec_old_handler		; 0x4C
	bra.w	.exec_old_handler		; 0x4D
	bra.w	.Fsfirst			; 0x4E Fsfirst
	bra.w	.Fsnext				; 0x4F Fsnext
	bra.w	.exec_old_handler		; 0x50
	bra.w	.exec_old_handler		; 0x51
	bra.w	.exec_old_handler		; 0x52
	bra.w	.exec_old_handler		; 0x53
	bra.w	.exec_old_handler		; 0x54
	bra.w	.exec_old_handler		; 0x55
	bra.w	.Frename			; 0x56 Frename
	bra.w	.Fdatime			; 0x57 Fdatime

; ====================================================================
; .Dfree — get free space on a drive
;
; Args: 8(a0) = pointer to DISKINFO struct (4 × u32), 12(a0) = drive
; (0 = current, 1 = A, 2 = B, ...).
; If the drive isn't ours, fall through to the original handler.
; Otherwise ask the RP, copy the four 32-bit fields into the caller's
; buffer, and return.
; ====================================================================
.Dfree:
	clr.l	d3
	move.l	8(a0), d4			; DISKINFO struct address
	move.w	12(a0), d3			; drive number arg (0 = current)
	tst.w	d3
	bne.s	.Dfree_real_drive
	; "0" means current drive — call Dgetdrv with reentry locked.
	reentry_gem_lock
	move.w	#GEMDOS_Dgetdrv, -(sp)
	trap	#1
	addq.l	#2, sp
	move.l	d0, -(sp)
	reentry_gem_unlock
	move.l	(sp)+, d3
	addq.l	#1, d3				; pretend caller passed (drive+1)
.Dfree_real_drive:
	subq.l	#1, d3				; back to 0-based for comparison
	cmp.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d3
	bne	.exec_old_handler
	send_sync CMD_DFREE_CALL, 2
	move.l	GEMDRIVE_DFREE_STATUS, d0
	tst.l	d0
	bne.s	.Dfree_exit
	; Status OK — copy DISKINFO from the shared struct.
	lea	GEMDRIVE_DFREE_STRUCT, a5
	move.l	d4, a4
	move.l	(a5)+, (a4)+			; total free clusters
	move.l	(a5)+, (a4)+			; clusters per drive
	move.l	(a5)+, (a4)+			; bytes per sector
	move.l	(a5)+, (a4)+			; sectors per cluster
.Dfree_exit:
	return_rte

; ====================================================================
; .Dgetpath — read current directory of a drive into the caller buffer
;
; Args: 8(a0) = pointer to caller buffer (assumed >= 128 bytes),
;       12(a0) = drive number (0 = current, 1 = A, 2 = B, ...).
; The RP keeps the live default-path string in shared memory at
; GEMDRIVE_DEFAULT_PATH; we ask it to refresh that string for our
; drive, then memcpy it into the caller's buffer.
; ====================================================================
.Dgetpath:
	clr.l	d3
	move.l	8(a0), a4			; caller buffer
	move.w	12(a0), d3			; drive number
	tst.w	d3
	beq.s	.Dgetpath_current
	subq.l	#1, d3
	cmp.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d3
	bne	.exec_old_handler
.Dgetpath_current:
	send_sync CMD_DGETPATH_CALL, 2
	move.w	#GEMDRIVE_DEFAULT_PATH_LEN-1, d0
	lea	GEMDRIVE_DEFAULT_PATH, a5
.Dgetpath_copy:
	tst.b	(a5)
	beq.s	.Dgetpath_done
	move.b	(a5)+, (a4)+
	dbf	d0, .Dgetpath_copy
.Dgetpath_done:
	move.b	#0, (a4)
	moveq.l	#0, d0
	return_rte

; ====================================================================
; .Dcreate — create a directory. Direct port of md-drives-emulator.
; ====================================================================
.Dcreate:
	move.l	8(a0), a4
	detect_emulated_drive_letter
	send_write_sync CMD_DCREATE_CALL, 256
	return_interrupt_w GEMDRIVE_DCREATE_STATUS

; ====================================================================
; .Ddelete — delete a directory. Direct port.
; ====================================================================
.Ddelete:
	move.l	8(a0), a4
	detect_emulated_drive_letter
	send_write_sync CMD_DDELETE_CALL, 256
	return_interrupt_w GEMDRIVE_DDELETE_STATUS

; ====================================================================
; .Fcreate — create a new file with attribs. Returns handle in d0.
; ====================================================================
.Fcreate:
	move.l	8(a0), a4
	move.w	12(a0), d3
	detect_emulated_drive_letter
	send_write_sync CMD_FCREATE_CALL, 256
	return_interrupt_w GEMDRIVE_FCREATE_HANDLE

; ====================================================================
; .Fdelete — delete a file. Direct port.
; ====================================================================
.Fdelete:
	move.l	8(a0), a4
	detect_emulated_drive_letter
	send_write_sync CMD_FDELETE_CALL, 256
	return_interrupt_w GEMDRIVE_FDELETE_STATUS

; ====================================================================
; .Fattrib — get/set attribute byte. Direct port.
; Args: 8(a0)=path, 12(a0)=mode (0=read,1=write), 14(a0)=attribs
; ====================================================================
.Fattrib:
	move.l	8(a0), a4
	move.w	12(a0), d3
	move.w	14(a0), d4
	detect_emulated_drive_letter
	send_write_sync CMD_FATTRIB_CALL, 128
	return_interrupt_l GEMDRIVE_FATTRIB_STATUS

; ====================================================================
; .Fdatime — get/set a file's date/time. Direct port of source.
; Args: 8(a0)=struct ptr, 12(a0)=handle, 14(a0)=flag (0=get, 1=set).
; ====================================================================
.Fdatime:
	move.l	8(a0), a4
	move.w	12(a0), d4
	move.w	14(a0), d3
	move.l	0(a4), d5
	move.l	4(a4), d6
	and.l	#$FFFF, d3
	and.l	#$FFFF, d4

	detect_emulated_drive_letter

	move.l	a4, -(sp)
	send_sync CMD_FDATETIME_CALL, 16
	move.l	(sp)+, a4

	lea	GEMDRIVE_FDATETIME_TIME, a6
	move.b	2(a6), 0(a4)
	move.b	3(a6), 1(a4)
	lea	GEMDRIVE_FDATETIME_DATE, a6
	move.b	2(a6), 2(a4)
	move.b	3(a6), 3(a4)

	return_interrupt_l GEMDRIVE_FDATETIME_STATUS

; ====================================================================
; .Frename — rename a file/dir. Args: 10(a0)=src, 14(a0)=dst.
; Direct port — packs both names back-to-back into a 256-byte buffer.
; ====================================================================
.Frename:
	move.l	10(a0), a5
	move.l	14(a0), a6

	detect_emulated_drive_letter

	lea	-256(sp), sp
	move.l	sp, a4

	move.w	#127, d3
.frename_copy_src:
	move.b	(a5)+, (a4)+
	dbf	d3, .frename_copy_src

	move.w	#127, d3
.frename_copy_dst:
	move.b	(a6)+, (a4)+
	dbf	d3, .frename_copy_dst

	move.l	sp, a4
	send_write_sync CMD_FRENAME_CALL, 256
	lea	256(sp), sp

	return_interrupt_l GEMDRIVE_FRENAME_STATUS

; ====================================================================
; .Fwrite — write to a GEMDRIVE-owned file. Direct port.
; Args: 8(a0)=handle.w, 10(a0)=count.l, 14(a0)=buffer ptr
;
; Streams BUFFER_WRITE_SIZE chunks via CMD_WRITE_BUFF_CALL. Each chunk:
; m68k pushes BUFFER_WRITE_SIZE bytes from the caller buffer; RP writes
; to the open file and reports actual bytes-written via
; GEMDRIVE_WRITE_BYTES; m68k advances and loops.
; ====================================================================
.Fwrite:
	move.w	8(a0), d3			; handle
	move.l	10(a0), d4			; bytes to write
	move.l	14(a0), a4			; src buffer

	detect_emulated_file_handler

	tst.l	d4
	bne.s	.fwrite_loop_init
	moveq.l	#0, d0				; nothing to write → return 0
	return_rte

.fwrite_loop_init:
	clr.l	d6				; bytes-written counter
.fwrite_loop:
	move.l	d4, d5
	cmp.l	#BUFFER_WRITE_SIZE, d5
	ble.s	.fwrite_chunk_size_ok
	move.l	#BUFFER_WRITE_SIZE, d5
.fwrite_chunk_size_ok:
	move.w	#CMD_RETRIES_COUNT, d7
.fwrite_retry:
	movem.l	d1-d7/a4, -(sp)
	move.w	#CMD_WRITE_BUFF_CALL, d0
	move.l	d5, d6
	bsr	send_sync_write_command_to_sidecart
	movem.l	(sp)+, d1-d7/a4
	tst.w	d0
	beq.s	.fwrite_chunk_ok
	dbf	d7, .fwrite_retry
	moveq.l	#GEMDOS_EIO_WRITE, d0
	return_rte

.fwrite_chunk_ok:
	move.l	GEMDRIVE_WRITE_BYTES, d2	; bytes RP actually wrote
	add.l	d2, a4
	add.l	d2, d6
	sub.l	d2, d4
	beq.s	.fwrite_done
	bpl	.fwrite_loop

.fwrite_done:
	move.l	d6, d0
	return_rte

; ====================================================================
; .Dsetpath — set the current working directory on the emulated drive.
; Args: 8(a0) = pointer to the new path string.
; Direct port from md-drives-emulator/target/atarist/src/gemdrive.s.
; ====================================================================
.Dsetpath:
	move.l	8(a0), a4

	detect_emulated_drive

	send_write_sync CMD_DSETPATH_CALL, 256

	return_interrupt_w GEMDRIVE_SET_DPATH_STATUS

; ====================================================================
; .Fopen — open file on the emulated drive
;
; Args: 8(a0) = pointer to filename string, 12(a0) = mode word.
; If the path is not on our drive, fall through. Otherwise ship the
; path (256 bytes from the caller buffer) plus the mode in d3 to the
; RP, which performs f_open and writes the resulting handle (or
; negative GEMDOS error) to GEMDRIVE_FOPEN_HANDLE.
; ====================================================================
.Fopen:
	move.l	8(a0), a4			; filename ptr
	move.w	12(a0), d3			; mode

	detect_emulated_drive_letter

	send_write_sync CMD_FOPEN_CALL, 256

	return_interrupt_l GEMDRIVE_FOPEN_HANDLE

; ====================================================================
; .Fclose — close a file owned by GEMDRIVE
; Args: 8(a0) = handle word.
; ====================================================================
.Fclose:
	move.w	8(a0), d3
	and.l	#$FFFF, d3

	detect_emulated_file_handler

	send_sync CMD_FCLOSE_CALL, 2

	return_interrupt_w GEMDRIVE_FCLOSE_STATUS

; ====================================================================
; .Fseek — seek in a GEMDRIVE-owned file
; Args: 8(a0) = offset.l, 12(a0) = handle.w, 14(a0) = mode.w
; ====================================================================
.Fseek:
	move.l	8(a0), d4			; offset
	move.w	12(a0), d3			; handle
	move.w	14(a0), d5			; mode

	detect_emulated_file_handler

	send_sync CMD_FSEEK_CALL, 12

	return_interrupt_l GEMDRIVE_FSEEK_STATUS

; ====================================================================
; .Fread — read bytes from a GEMDRIVE-owned file
; Args: 8(a0) = handle.w, 10(a0) = count.l, 14(a0) = buffer ptr
;
; Streams BUFFER_READ_SIZE-byte chunks from the RP via
; CMD_READ_BUFF_CALL. Each iteration: send (handle, count_remaining,
; pending), the RP fills GEMDRIVE_READ_BUFFER and writes signed
; bytes-read into GEMDRIVE_READ_BYTES, and we copy that many bytes
; into the caller's buffer. Loop until done or EOF.
; ====================================================================
.Fread:
	move.w	8(a0), d3			; handle
	move.l	10(a0), d4			; bytes requested
	move.l	14(a0), a4			; caller dest buffer

	detect_emulated_file_handler

	bsr	.Fread_core
	return_rte

; ====================================================================
; .Fread_core — callable subroutine reused by .Fread and .Pexec.
; In:  d3.w = handle, d4.l = bytes wanted, a4 = dest buffer
; Out: d0.l = bytes read or negative GEMDOS error
; Trashes: d0-d7, a4-a5
; ====================================================================
.Fread_core:
	move.l	d4, d5				; d5 = total bytes wanted
	moveq.l	#0, d6				; d6 = bytes accumulated
.Fread_core_loop:
	send_sync CMD_READ_BUFF_CALL, 12

	tst.w	d0
	beq.s	.Fread_core_cmd_ok
	moveq.l	#GEMDOS_EIO_READ, d0
	rts

.Fread_core_cmd_ok:
	move.l	GEMDRIVE_READ_BYTES, d0
	bmi.s	.Fread_core_done		; negative => error
	tst.l	d0
	beq.s	.Fread_core_eof			; zero => EOF

	movem.l	d0/d6-d7, -(sp)
	move.l	d0, d7
	subq.l	#1, d7
	lea	GEMDRIVE_READ_BUFFER, a5
.Fread_core_copy:
	move.b	(a5)+, (a4)+
	dbf	d7, .Fread_core_copy
	movem.l	(sp)+, d0/d6-d7

	add.l	d0, d6
	cmp.l	#BUFFER_READ_SIZE, d0
	bne.s	.Fread_core_eof
	sub.l	d0, d5
	bgt.s	.Fread_core_loop

.Fread_core_eof:
	move.l	d6, d0
.Fread_core_done:
	rts

; ====================================================================
; .fill_zero — zero d5.l bytes starting at a5. Trashes a5/d5.
; ====================================================================
.fill_zero:
	tst.l	d5
	beq.s	.fill_zero_exit
.fill_zero_loop:
	clr.b	(a5)+
	subq.l	#1, d5
	bne.s	.fill_zero_loop
.fill_zero_exit:
	rts

; ====================================================================
; .Fsetdta — record the caller's DTA address with the RP so Fsfirst /
; Fsnext can look it up by address. The original handler still runs so
; TOS keeps its own _dta variable too (.exec_old_handler).
; ====================================================================
.Fsetdta:
	move.l	8(a0), d3
	; Match source: only register the DTA when the current drive is the
	; emulated one. Otherwise we'd burn DTA-table slots on every A:/B:
	; Fsetdta the desktop issues.
	detect_emulated_drive
	send_sync CMD_FSETDTA_CALL, 4
	bra	.exec_old_handler

; ====================================================================
; .Pexec — load and execute a PRG file. Direct port of
; md-drives-emulator/target/atarist/src/gemdrive.s with the TOS 1.00
; PE_GO trampoline removed (we always assume PE_GO_AND_FREE works,
; i.e. TOS 1.04+ — the user can ramp this back if old TOS support
; matters).
;
; Args (on the args frame at a0):
;   8(a0)   mode.w   (0=LOAD_GO, 3=LOAD only, 4=GO, 5=BASEPAGE, 6=GOFREE)
;   10(a0)  fname ptr
;   14(a0)  cmdline ptr
;   18(a0)  envstr ptr
; ====================================================================
.Pexec:
	move.l	a0, d3
	move.l	a0, a4

	detect_emulated_drive_letter

	send_write_sync CMD_PEXEC_CALL, 32

	cmp.w	#PE_LOAD_GO, GEMDRIVE_PEXEC_MODE
	beq.s	.pexec_load_go
	cmp.w	#PE_LOAD, GEMDRIVE_PEXEC_MODE
	bne	.exec_old_handler

.pexec_load_go:
	; Open the program file via Fopen (read).
	move.l	GEMDRIVE_PEXEC_FNAME, a4
	clr.w	d3
	send_write_sync CMD_FOPEN_CALL, 256
	move.l	GEMDRIVE_FOPEN_HANDLE, d0
	bmi	.pexec_exit

	; Read the 28-byte PRG header into a transient stack buffer.
	move.w	d0, d3				; handle
	move.l	#PRG_STRUCT_SIZE, d4
	lea	-32(sp), sp
	move.l	sp, a4
	bsr	.Fread_core
	move.l	sp, a4
	cmp.l	#PRG_STRUCT_SIZE, d0
	bne	.pexec_close_exit_fix_hdr_buf
	cmp.w	#PRG_MAGIC_NUMBER, 0(a4)
	bne	.pexec_close_exit_fix_hdr_buf

	; Ship the header to the RP for stash.
	send_write_sync CMD_SAVE_EXEC_HEADER, $1c
	lea	32(sp), sp

	; Re-enter GEMDOS in PE_CREATE_BASEPAGE mode to ask TOS for a
	; basepage backed by enough TPA for text/data/bss.
	reentry_gem_lock
	move.l	GEMDRIVE_PEXEC_ENVSTR, -(sp)
	move.l	GEMDRIVE_PEXEC_CMDLINE, -(sp)
	clr.l	-(sp)
	move.w	#PE_CREATE_BASEPAGE, -(sp)
	move.w	#GEMDOS_Pexec, -(sp)
	trap	#1
	lea	16(sp), sp
	move.l	d0, a4				; a4 = basepage addr
	reentry_gem_unlock

	; Patch the basepage's text/data/bss segment pointers from the header.
	lea	GEMDRIVE_EXEC_HEADER, a5
	move.l	2(a5), d3			; text size
	move.l	6(a5), d4			; data size
	move.l	10(a5), d5			; bss size
	move.l	14(a5), d6			; symbol size

	move.l	a4, d7
	add.l	#$100, d7			; basepage + 256 = text base
	move.l	d7, 8(a4)
	move.l	d3, 12(a4)
	add.l	d3, d7
	move.l	d7, 16(a4)
	move.l	d4, 20(a4)
	add.l	d4, d7
	move.l	d7, 24(a4)
	move.l	d5, 28(a4)

	; Ship the basepage to the RP for relocation reference.
	send_write_sync CMD_SAVE_BASEPAGE, 256

	; Read the rest of the PRG file straight into the text segment.
	lea	GEMDRIVE_EXEC_HEADER, a5
	lea	GEMDRIVE_EXEC_PD, a4
	move.l	8(a4), a4
	move.l	2(a5), d4
	add.l	6(a5), d4
	add.l	14(a5), d4
	add.l	#$FFFF, d4

	move.l	GEMDRIVE_FOPEN_HANDLE, d3
	bsr	.Fread_core

	; Close the file.
	move.l	GEMDRIVE_FOPEN_HANDLE, d3
	send_sync CMD_FCLOSE_CALL, 2
	move.w	GEMDRIVE_FCLOSE_STATUS, d0
	ext.l	d0
	bmi	.pexec_exit

	; Walk the relocation table and apply fixups (standard PRG format).
	lea	GEMDRIVE_EXEC_PD, a5
	move.l	8(a5), a5			; text base
	move.l	a5, d1
	move.l	a5, a6

	lea	GEMDRIVE_EXEC_HEADER, a4
	add.l	2(a4), a5
	add.l	6(a4), a5
	add.l	14(a4), a5
	tst.l	(a5)
	beq.s	.pexec_zero_bss_no_reloc
	moveq	#0, d0
	add.l	(a5)+, a6
.pexec_fixup_apply:
	add.l	d1, (a6)
.pexec_fixup_next:
	move.b	(a5)+, d0
	beq.s	.pexec_zero_bss
	cmp.b	#1, d0
	bne.s	.pexec_fixup_skip_bump
	add.w	#$fe, a6
	bra.s	.pexec_fixup_next
.pexec_fixup_skip_bump:
	add.w	d0, a6
	bra.s	.pexec_fixup_apply

.pexec_zero_bss_no_reloc:
.pexec_zero_bss:
	move.l	GEMDRIVE_EXEC_PD, a4
	move.l	24(a4), a5
	move.l	28(a4), d5
	bsr	.fill_zero

	; PE_LOAD: just return the basepage.
	cmp.w	#PE_LOAD, GEMDRIVE_PEXEC_MODE
	beq	.pexec_exit_load

	; PE_LOAD_GO: rewrite the caller's args frame to PE_GO_AND_FREE
	; semantics and fall back to old_handler so TOS does the actual jump.
	move.l	GEMDRIVE_PEXEC_STACK_ADDR, a0
	move.w	#PE_GO_AND_FREE, 8(a0)
	clr.l	10(a0)
	move.l	GEMDRIVE_EXEC_PD, 14(a0)
	clr.l	18(a0)
	bra	.exec_old_handler

.pexec_exit_load:
	move.l	GEMDRIVE_EXEC_PD, d0
.pexec_exit:
	return_rte

.pexec_close_exit_fix_hdr_buf:
	lea	32(sp), sp
	move.l	GEMDRIVE_FOPEN_HANDLE, d3
	send_sync CMD_FCLOSE_CALL, 2
	move.w	GEMDRIVE_FCLOSE_STATUS, d0
	ext.l	d0
	bra	.pexec_exit

; ====================================================================
; .Fsfirst — start a directory iteration
; Args: 8(a0) = pattern ptr, 12(a0) = attribs.w
;
; Calls Fgetdta (with reentry locked) to learn the caller's DTA
; address, ships the pattern + DTA pointer + attribs to the RP, and
; copies the populated DTA back into the caller's DTA buffer.
; ====================================================================
.Fsfirst:
	move.l	8(a0), a4			; pattern
	move.w	12(a0), d4			; attribs

	; Drive-letter check (manual: source's macro doesn't fully fit).
	cmp.b	#':', 1(a4)
	bne.s	.Fsfirst_check_current
	move.b	(a4), d0
	cmp.b	(SHARED_VARIABLES+3+(SHARED_VAR_DRIVE_LETTER*4)), d0
	bne	.exec_old_handler
	bra.s	.Fsfirst_get_dta
.Fsfirst_check_current:
	detect_emulated_drive

.Fsfirst_get_dta:
	reentry_gem_lock
	move.w	#GEMDOS_Fgetdta, -(sp)
	trap	#1
	addq.l	#2, sp
	move.l	d0, -(sp)
	reentry_gem_unlock

	move.l	(sp), d3			; d3 = caller DTA addr
	move.l	a4, d5				; d5 = pattern addr
	send_write_sync CMD_FSFIRST_CALL, 192

	bra	.populate_fsdta

; ====================================================================
; .Fsnext — continue a directory iteration
; Args: implicit, the current DTA from Fgetdta. We re-fetch it and ship
; it as d3 so the RP can look up the iteration state by DTA address.
; ====================================================================
.Fsnext:
	reentry_gem_lock
	move.w	#GEMDOS_Fgetdta, -(sp)
	trap	#1
	addq.l	#2, sp
	move.l	d0, -(sp)
	reentry_gem_unlock

	; If the DTA isn't on our drive, bail. The DTA's "drive" byte was
	; written at offset 12 of the DTA when Fsfirst was called.
	move.l	(sp), a0
	move.l	12(a0), d0
	cmp.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d0
	bne.s	.Fsnext_bypass

	move.l	(sp), d3
	send_sync CMD_FSNEXT_CALL, 4

	bra.s	.populate_fsdta

.Fsnext_bypass:
	move.l	(sp)+, d0
	bra	.exec_old_handler

; ====================================================================
; .populate_fsdta — shared tail for Fsfirst/Fsnext
;
; The caller pushed the DTA address; we copy DTA_SIZE bytes from the
; shared region into it (or zero it on a miss), set GEMDOS Dsetdrv to
; our drive (TOS expects the directory walk to leave the emulated
; drive current), and rte with the appropriate status.
; ====================================================================
.populate_fsdta:
	move.l	(sp)+, a5			; caller DTA addr
	move.w	GEMDRIVE_DTA_F_FOUND, d0
	tst.w	d0
	bne.s	.populate_fsdta_empty

	lea	GEMDRIVE_DTA_TRANSFER, a4
	moveq.l	#DTA_SIZE-1, d2
.populate_fsdta_copy:
	move.b	(a4)+, (a5)+
	dbf	d2, .populate_fsdta_copy

	; Make our drive the current GEMDOS drive on success.
	reentry_gem_lock
	move.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d0
	move.w	d0, -(sp)
	move.w	#GEMDOS_Dsetdrv, -(sp)
	trap	#1
	addq.l	#4, sp
	reentry_gem_unlock

	moveq.l	#0, d0
	return_rte

.populate_fsdta_empty:
	move.l	d0, -(sp)
	move.l	a5, d3
	moveq.l	#DTA_SIZE-1, d2
.populate_fsdta_zero:
	clr.b	(a5)+
	dbf	d2, .populate_fsdta_zero

	send_sync CMD_DTA_RELEASE_CALL, 4

	reentry_gem_lock
	move.l	(SHARED_VARIABLES+(SHARED_VAR_DRIVE_NUMBER*4)), d0
	move.w	d0, -(sp)
	move.w	#GEMDOS_Dsetdrv, -(sp)
	trap	#1
	addq.l	#4, sp
	reentry_gem_unlock

	move.l	(sp)+, d0
	ext.l	d0
	return_rte

; Pull in the shared protocol functions LOCAL to gemdrive.o so the
; entire request path lives inside the relocated blob and runs from
; RAM. Same approach as md-drives-emulator's per-module .s files.
	include	"inc/sidecart_functions.s"

; End-of-file sentinel.
gemdrive_end:
	even
	dc.l $DEADFFFF
	dc.l 0
