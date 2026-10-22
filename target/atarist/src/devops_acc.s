; SidecarTridge Multi-device DevOps — Runner desk accessory (DEVOPS.ACC)
; License: GPL v3
;
; Serves the Runner's foreground commands (run, load, exec, unload, cd)
; while the ST sits at the GEM desktop, in TSR mode ([G] in the setup
; menu with the GEMDRIVE Runner on). The cartridge installs GEMDRIVE and the Runner's interrupt hook
; and lets TOS boot on through the AUTO folder; GEM then starts this
; accessory from the root of the boot drive. Every POLL_MS it reads the
; cartridge sentinel the RP writes commands to, and answers with the same
; report commands the cartridge Runner sends, so the RP treats both alike.
;
; GEM only gives an accessory a turn while the foreground program waits
; for events, so GEMDOS is never in the middle of a call when this code
; runs. A program launched from here runs as a child of the desktop and
; takes over the screen: the accessory locks the screen first and gives
; the desktop its screen back afterwards. GEM programs are not supported:
; on single-tasking TOS one started from an accessory would run inside
; the accessory's AES slot.
;
; GEMDOS calls made here belong to the desktop's process, so the current
; drive and directory are the desktop's too. The accessory keeps the
; Runner's cwd itself and sets it again before every command.
;
; Trace lines go to the debug-capture window ($FBFF00 + byte), so they
; reach `sidecart debug tail` and the USB serial port, not the screen.
; The Desk menu entry opens an alert with what the accessory knows
; itself: the Runner's cwd and the last command it served.
;
; Unlike the cartridge modules this is an ordinary TOS executable,
; loaded and relocated by GEM, so it may use absolute addresses.

; --- Shared region (must match rp/src/include/chandler.h, runner.h) ---
ROM4_ADDR		equ $FA0000
CARTRIDGE_CODE_SIZE	equ $2800
SHARED_BLOCK_ADDR	equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)	; $FA2800
CMD_MAGIC_SENTINEL_ADDR	equ SHARED_BLOCK_ADDR
SHARED_VARIABLES_ADDR	equ (SHARED_BLOCK_ADDR + $10)
APP_FREE_ADDR		equ (SHARED_BLOCK_ADDR + $300)
DRIVE_NUMBER_ADDR	equ (SHARED_VARIABLES_ADDR + 12*4)	; GEMDRIVE drive, 0 = A:
RUNNER_BASE		equ (APP_FREE_ADDR + $1800)
RUNNER_PATH		equ (RUNNER_BASE + $000)		; NUL-terminated
RUNNER_CMDLINE		equ (RUNNER_BASE + $080)		; TOS length-prefixed
RUNNER_BASEPAGE		equ (RUNNER_BASE + $194)		; staged for Pexec(4) / Mfree
DEBUG_BASE		equ $FBFF00				; read $FBFF00 + c to emit c

; --- Constants inc/sidecart_functions.s needs (same values as runner.s) ---
RANDOM_TOKEN_ADDR	equ (SHARED_BLOCK_ADDR + 4)
RANDOM_TOKEN_SEED_ADDR	equ (SHARED_BLOCK_ADDR + 8)
RANDOM_TOKEN_POST_WAIT	equ $1
ROMCMD_START_ADDR	equ $FB0000
CMD_MAGIC_NUMBER	equ $ABCD
CMD_RETRIES_COUNT	equ 5
COMMAND_TIMEOUT		equ $0006FFFF
COMMAND_WRITE_TIMEOUT	equ COMMAND_TIMEOUT
_dskbufp		equ $4C6

; --- Runner protocol (must match rp/src/include/runner.h) ---
APP_RUNNER			equ $0500
RUNNER_CMD_EXECUTE		equ (APP_RUNNER + $02)	; Pexec mode 0
RUNNER_CMD_CD			equ (APP_RUNNER + $03)	; Dsetpath
RUNNER_CMD_LOAD			equ (APP_RUNNER + $06)	; Pexec mode 3
RUNNER_CMD_EXEC			equ (APP_RUNNER + $07)	; Pexec mode 4
RUNNER_CMD_UNLOAD		equ (APP_RUNNER + $08)	; Mfree(basepage)
RUNNER_CMD_DONE_EXECUTE		equ (APP_RUNNER + $82)	; i32 exit code
RUNNER_CMD_DONE_CD		equ (APP_RUNNER + $83)	; i32 GEMDOS errno
RUNNER_CMD_DONE_LOAD		equ (APP_RUNNER + $87)	; i32 basepage or -errno
RUNNER_CMD_DONE_EXEC		equ (APP_RUNNER + $88)	; i32 exit code
RUNNER_CMD_DONE_UNLOAD		equ (APP_RUNNER + $89)	; i32 Mfree result
RUNNER_CMD_DONE_ACC_HELLO	equ (APP_RUNNER + $8A)	; no payload

PE_LOAD_GO		equ 0
PE_LOAD			equ 3
PE_GO			equ 4

; --- XBIOS ---
XB_PHYSBASE		equ 2
XB_LOGBASE		equ 3
XB_GETREZ		equ 4
XB_SETSCREEN		equ 5
XB_SETPALETTE		equ 6
XB_SETCOLOR		equ 7
XB_SUPEXEC		equ 38

; --- AES ---
APPL_INIT		equ 10
EVNT_MULTI		equ 25
MENU_REGISTER		equ 35
FORM_DIAL		equ 51
FORM_ALERT		equ 52
GRAF_MOUSE		equ 78
WIND_GET		equ 104
WIND_UPDATE		equ 107
END_UPDATE		equ 0
BEG_UPDATE		equ 1
END_MCTRL		equ 2
BEG_MCTRL		equ 3
M_OFF			equ 256
M_ON			equ 257
WF_WORKXYWH		equ 4
FMD_FINISH		equ 3
MU_MESAG		equ $10
MU_TIMER		equ $20
AC_OPEN			equ 40

POLL_MS			equ 100
STRIP_MAX		equ 3200	; menu bar strip: up to 20 rows of 160 bytes
CWD_SHOWN		equ 23		; cwd characters that fit an alert line

	include	"inc/sidecart_macros.s"
	include	"inc/tos.s"

; AES call. \1 opcode, then the int_in, int_out, addr_in and addr_out
; counts. Arguments go in int_in / addr_in first; trashes d0-d2/a0-a2.
aes	macro
	lea	control, a0
	move.w	#\1, (a0)+
	move.w	#\2, (a0)+
	move.w	#\3, (a0)+
	move.w	#\4, (a0)+
	move.w	#\5, (a0)
	move.l	#aes_pb, d1
	move.w	#$C8, d0
	trap	#2
	endm

; Report result to the RP: \1 report command, \2 the command it answers,
; which is also what the status alert shows as the last command. The RP
; clears the sentinel before it acknowledges, so only a report that timed
; out leaves the command there; remember it so poll does not run it a
; second time.
report	macro
	move.l	#\2, last_cmd
	move.l	result, d3
	send_sync \1, 4
	tst.w	d0
	beq.s	.\@acked
	move.l	#\2, unacked_cmd
.\@acked:
	endm

; Emit the NUL-terminated string at \1 to the debug stream.
trace	macro
	lea	\1(pc), a1
	bsr	debug_puts
	endm

	section	text,code

acc_start:
	; GEM starts an accessory with its basepage in a0 and no usable
	; stack. Started from the desktop as a program, a0 is 0: just end.
	move.l	a0, d0
	bne.s	.is_accessory
	Pterm0
.is_accessory:
	lea	acc_stack_top, sp

	aes	APPL_INIT, 0, 1, 0, 0
	move.w	int_out, int_in			; ap_id
	move.l	#text_menu_entry, addr_in
	aes	MENU_REGISTER, 1, 1, 1, 0

	bsr	cache_off
	send_sync RUNNER_CMD_DONE_ACC_HELLO, 0
	trace	text_ready
	bsr	cache_back

	; Wake on the timer to poll, and on a message: a registered
	; accessory must read its messages, and AC_OPEN is the Desk menu
	; entry being clicked.
.loop:
	lea	int_in, a0
	move.w	#MU_MESAG+MU_TIMER, (a0)+
	moveq	#13-1, d0			; no buttons, no mouse rectangles
.clear:
	clr.w	(a0)+
	dbf	d0, .clear
	move.w	#POLL_MS, (a0)+
	clr.w	(a0)
	move.l	#msg_buf, addr_in
	aes	EVNT_MULTI, 16, 7, 1, 0
	btst	#4, int_out+1			; MU_MESAG
	beq.s	.poll
	cmp.w	#AC_OPEN, msg_buf
	bne.s	.poll
	bsr	show_status
.poll:
	bsr	cache_off
	bsr	poll
	bsr	cache_back
	bra.s	.loop

; Read the sentinel and serve a foreground Runner command. $06xx commands
; belong to the cartridge's interrupt hook and are left alone.
poll:
	move.l	CMD_MAGIC_SENTINEL_ADDR, d6
	move.l	unacked_cmd, d0
	beq.s	.dispatch
	cmp.l	d0, d6
	beq.s	.done
	clr.l	unacked_cmd
.dispatch:
	cmp.l	#RUNNER_CMD_EXECUTE, d6
	beq	do_execute
	cmp.l	#RUNNER_CMD_LOAD, d6
	beq	do_load
	cmp.l	#RUNNER_CMD_EXEC, d6
	beq	do_exec
	cmp.l	#RUNNER_CMD_UNLOAD, d6
	beq	do_unload
	cmp.l	#RUNNER_CMD_CD, d6
	beq	do_cd
.done:
	rts

; Pexec(0) RUNNER_PATH with RUNNER_CMDLINE.
do_execute:
	trace	text_run
	lea	RUNNER_PATH, a1
	bsr	debug_puts
	trace	text_crlf

	bsr	enter_cwd
	bsr	take_screen
	bsr	cache_back			; the program gets the desktop's cache
	clr.l	-(sp)				; environment: inherit
	pea	RUNNER_CMDLINE
	pea	RUNNER_PATH
	move.w	#PE_LOAD_GO, -(sp)
	move.w	#Pexec, -(sp)
	trap	#1
	lea	16(sp), sp
	move.l	d0, result
	bsr	cache_off
	bsr	give_screen_back

	bsr	trace_exit
	lea	RUNNER_PATH, a1
	bsr	debug_puts
	trace	text_terminated

	report	RUNNER_CMD_DONE_EXECUTE, RUNNER_CMD_EXECUTE
	rts

; Pexec(3): load only. The program keeps the cwd it was loaded with.
do_load:
	trace	text_load
	lea	RUNNER_PATH, a1
	bsr	debug_puts
	trace	text_crlf

	bsr	enter_cwd
	clr.l	-(sp)
	pea	RUNNER_CMDLINE
	pea	RUNNER_PATH
	move.w	#PE_LOAD, -(sp)
	move.w	#Pexec, -(sp)
	trap	#1
	lea	16(sp), sp
	move.l	d0, result

	trace	text_load_result
	move.l	result, d3
	bsr	debug_dec
	trace	text_crlf

	report	RUNNER_CMD_DONE_LOAD, RUNNER_CMD_LOAD
	rts

; Pexec(4) the basepage a previous load returned. The basepage goes in
; the command-line slot.
do_exec:
	trace	text_exec
	move.l	RUNNER_BASEPAGE, d3
	bsr	debug_dec
	trace	text_crlf

	bsr	take_screen
	move.l	RUNNER_BASEPAGE, d3
	bsr	cache_back			; the program gets the desktop's cache
	clr.l	-(sp)
	move.l	d3, -(sp)
	clr.l	-(sp)
	move.w	#PE_GO, -(sp)
	move.w	#Pexec, -(sp)
	trap	#1
	lea	16(sp), sp
	move.l	d0, result
	bsr	cache_off
	bsr	give_screen_back

	bsr	trace_exit
	trace	text_terminated

	report	RUNNER_CMD_DONE_EXEC, RUNNER_CMD_EXEC
	rts

; Mfree the loaded program's basepage.
do_unload:
	trace	text_unload
	move.l	RUNNER_BASEPAGE, d3
	bsr	debug_dec
	trace	text_crlf

	move.l	RUNNER_BASEPAGE, -(sp)
	move.w	#Mfree, -(sp)
	trap	#1
	addq.l	#6, sp
	move.l	d0, result

	trace	text_unload_result
	move.l	result, d3
	bsr	debug_dec
	trace	text_crlf

	report	RUNNER_CMD_DONE_UNLOAD, RUNNER_CMD_UNLOAD
	rts

; Dsetpath RUNNER_PATH, relative to the Runner's cwd, and keep the result
; as the new cwd.
do_cd:
	trace	text_cd
	lea	RUNNER_PATH, a1
	bsr	debug_puts
	trace	text_crlf

	bsr	enter_cwd
	pea	RUNNER_PATH
	move.w	#Dsetpath, -(sp)
	trap	#1
	addq.l	#6, sp
	move.l	d0, result
	bne.s	.reported			; failed: the cwd is unchanged

	clr.w	-(sp)				; current drive, set by enter_cwd
	pea	acc_cwd
	move.w	#Dgetpath, -(sp)
	trap	#1
	addq.l	#8, sp
	tst.b	acc_cwd				; root can come back empty
	bne.s	.reported
	move.b	#$5C, acc_cwd			; backslash
	clr.b	acc_cwd+1
.reported:
	report	RUNNER_CMD_DONE_CD, RUNNER_CMD_CD
	rts

; A Mega STE's cache answers the cartridge's reads from copies it made
; before, and the command channel breaks with it on, as it is on a desktop
; set up to run fast. cache_off turns it off while the accessory talks to
; the cartridge, and cache_back turns it on again if it was; a program the
; accessory starts runs between the two. The RP says which machine this
; is, from the boot: no other has the register.
cache_off:
	pea	cache_off_super(pc)
	bra.s	supexec
cache_back:
	pea	cache_back_super(pc)
supexec:
	move.w	#XB_SUPEXEC, -(sp)
	trap	#14
	addq.l	#6, sp
	rts

cache_off_super:
	cmp.l	#COOKIE_JAR_MEGASTE, SHARED_VARIABLES_ADDR+(SHARED_VARIABLE_HARDWARE_TYPE*4)
	bne.s	.no_register
	move.b	MEGASTE_SPEED_CACHE_REG.w, cache_setting
	bclr	#0, MEGASTE_SPEED_CACHE_REG.w
.no_register:
	rts

cache_back_super:
	btst	#0, cache_setting
	beq.s	.was_off
	bset	#0, MEGASTE_SPEED_CACHE_REG.w
.was_off:
	rts

; Current drive and directory back to the Runner's.
enter_cwd:
	move.l	DRIVE_NUMBER_ADDR, d0
	move.w	d0, -(sp)
	move.w	#Dsetdrv, -(sp)
	trap	#1
	addq.l	#4, sp
	pea	acc_cwd
	move.w	#Dsetpath, -(sp)
	trap	#1
	addq.l	#6, sp
	rts

; Hand the screen to a TOS program, as the desktop does when it launches
; one: lock out GEM, hide the mouse, clear the screen with the text cursor
; on. First save what the program may change and give_screen_back needs.
take_screen:
	move.w	#BEG_UPDATE, int_in
	aes	WIND_UPDATE, 1, 1, 0, 0
	move.w	#BEG_MCTRL, int_in
	aes	WIND_UPDATE, 1, 1, 0, 0
	move.w	#M_OFF, int_in
	clr.l	addr_in
	aes	GRAF_MOUSE, 1, 1, 1, 0

	; The desktop's work area, to redraw afterwards. Its y is the
	; height of the menu bar above it.
	lea	int_in, a0
	clr.w	(a0)+				; window 0: the desktop
	move.w	#WF_WORKXYWH, (a0)
	aes	WIND_GET, 2, 5, 0, 0
	move.l	int_out+2, desk_xywh
	move.l	int_out+6, desk_xywh+4

	move.w	#XB_PHYSBASE, -(sp)
	trap	#14
	addq.l	#2, sp
	move.l	d0, saved_phys
	move.w	#XB_LOGBASE, -(sp)
	trap	#14
	addq.l	#2, sp
	move.l	d0, saved_log
	move.w	#XB_GETREZ, -(sp)
	trap	#14
	addq.l	#2, sp
	move.w	d0, saved_rez

	moveq	#0, d3
	lea	saved_palette, a3
.palette:
	move.w	#-1, -(sp)			; read colour d3
	move.w	d3, -(sp)
	move.w	#XB_SETCOLOR, -(sp)
	trap	#14
	addq.l	#6, sp
	move.w	d0, (a3)+
	addq.w	#1, d3
	cmp.w	#16, d3
	bne.s	.palette

	; The desktop owns the menu bar and nothing can ask it to draw
	; it again, so keep a copy of its pixels.
	bsr	strip_bytes
	move.l	d0, strip_len
	beq.s	.cleared
	move.l	saved_log, a0
	lea	strip_buf, a1
	lsr.w	#1, d0
	subq.w	#1, d0
.save_strip:
	move.w	(a0)+, (a1)+
	dbf	d0, .save_strip
.cleared:
	pea	text_clear(pc)
	move.w	#Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	rts

; Give the screen back to GEM after the program: its screen, resolution
; and palette (Setscreen with a resolution also clears the screen), the
; text cursor off, the menu bar pixels, then a redraw of the desktop
; below it.
give_screen_back:
	move.w	#XB_GETREZ, -(sp)
	trap	#14
	addq.l	#2, sp
	moveq	#-1, d1				; resolution: unchanged
	cmp.w	saved_rez, d0
	beq.s	.same_rez
	move.w	saved_rez, d1
.same_rez:
	move.w	d1, -(sp)
	move.l	saved_phys, -(sp)
	move.l	saved_log, -(sp)
	move.w	#XB_SETSCREEN, -(sp)
	trap	#14
	lea	12(sp), sp

	pea	saved_palette
	move.w	#XB_SETPALETTE, -(sp)
	trap	#14
	addq.l	#6, sp

	pea	text_cursor_off(pc)
	move.w	#Cconws, -(sp)
	trap	#1
	addq.l	#6, sp

	move.l	strip_len, d0
	beq.s	.gem
	lea	strip_buf, a0
	move.l	saved_log, a1
	lsr.w	#1, d0
	subq.w	#1, d0
.restore_strip:
	move.w	(a0)+, (a1)+
	dbf	d0, .restore_strip

.gem:
	move.w	#M_ON, int_in
	clr.l	addr_in
	aes	GRAF_MOUSE, 1, 1, 1, 0
	move.w	#END_MCTRL, int_in
	aes	WIND_UPDATE, 1, 1, 0, 0
	move.w	#END_UPDATE, int_in
	aes	WIND_UPDATE, 1, 1, 0, 0

	lea	int_in, a0
	move.w	#FMD_FINISH, (a0)+
	clr.l	(a0)+				; small rectangle: unused
	clr.l	(a0)+
	move.l	desk_xywh, (a0)+		; area to redraw
	move.l	desk_xywh+4, (a0)
	aes	FORM_DIAL, 9, 1, 0, 0
	rts

; d0.l = size of the menu bar strip in screen memory, or 0 when the
; resolution is not an ST one or the strip will not fit in strip_buf.
strip_bytes:
	moveq	#0, d0
	move.w	desk_xywh+2, d0			; rows above the desktop
	move.w	saved_rez, d1
	cmp.w	#2, d1
	bhi.s	.none
	beq.s	.mono
	mulu.w	#160, d0			; low and medium: 160 bytes a row
	bra.s	.fits
.mono:
	mulu.w	#80, d0
.fits:
	cmp.l	#STRIP_MAX, d0
	bls.s	.done
.none:
	moveq	#0, d0
.done:
	rts

; The Desk menu entry's alert. GEM holds the accessory in form_alert
; until OK is clicked, so commands wait while it is open.
show_status:
	lea	alert_buf, a0
	lea	text_alert_head(pc), a1
	bsr	str_cat
	move.l	DRIVE_NUMBER_ADDR, d0
	add.b	#'A', d0
	move.b	d0, (a0)+
	move.b	#':', (a0)+
	lea	acc_cwd, a1
	moveq	#CWD_SHOWN, d1
	bsr	str_ncat
	lea	text_alert_last(pc), a1
	bsr	str_cat

	move.l	last_cmd, d0
	beq.s	.none
	lea	text_last_run(pc), a1
	cmp.w	#RUNNER_CMD_EXECUTE, d0
	beq.s	.named
	lea	text_last_load(pc), a1
	cmp.w	#RUNNER_CMD_LOAD, d0
	beq.s	.named
	lea	text_last_exec(pc), a1
	cmp.w	#RUNNER_CMD_EXEC, d0
	beq.s	.named
	lea	text_last_unload(pc), a1
	cmp.w	#RUNNER_CMD_UNLOAD, d0
	beq.s	.named
	lea	text_last_cd(pc), a1
.named:
	bsr	str_cat
	move.l	result, d3
	bsr	format_dec
	bsr	str_cat
	bra.s	.tail
.none:
	lea	text_last_none(pc), a1
	bsr	str_cat
.tail:
	lea	text_alert_tail(pc), a1
	bsr	str_cat

	move.w	#1, int_in			; default button: OK
	move.l	#alert_buf, addr_in
	aes	FORM_ALERT, 1, 1, 1, 0
	rts

; Append the string at a1 to a0, NUL included; a0 ends on the NUL.
str_cat:
	move.b	(a1)+, (a0)+
	bne.s	str_cat
	subq.l	#1, a0
	rts

; Append at most d1.w characters of the string at a1 to a0, no NUL.
str_ncat:
	subq.w	#1, d1
	bmi.s	.done
.next:
	move.b	(a1)+, d0
	beq.s	.done
	move.b	d0, (a0)+
	dbf	d1, .next
.done:
	rts

; "[EXIT n] - " for the exit code in result.
trace_exit:
	trace	text_exit_open
	move.l	result, d3
	bsr	debug_dec
	trace	text_exit_close
	rts

; Emit the NUL-terminated string at a1. Trashes d0, a0, a1.
debug_puts:
	lea	DEBUG_BASE, a0
.next:
	moveq	#0, d0
	move.b	(a1)+, d0
	beq.s	.done
	tst.b	(a0, d0.w)			; the read itself is the emit
	bra.s	.next
.done:
	rts

; Emit d3 as signed decimal. Trashes d0, d4, d5, a0, a1.
debug_dec:
	bsr.s	format_dec
	bra.s	debug_puts

; d3 as signed decimal: a1 = its NUL-terminated digits, in dec_buf. The
; 68000 only divides 32 by 16 bits, so each digit takes two divides: the
; high word, then the remainder with the low word. Trashes d0, d4, d5.
format_dec:
	lea	dec_buf+12, a1
	clr.b	-(a1)
	move.l	d3, d4
	bpl.s	.digit
	neg.l	d4
.digit:
	move.l	d4, d0
	clr.w	d0
	swap	d0
	divu.w	#10, d0				; rem1 : high / 10
	move.w	d0, d5
	move.w	d4, d0				; rem1 : low word
	divu.w	#10, d0				; digit : low quotient
	swap	d5
	move.w	d0, d5				; d5 = quotient
	swap	d0
	add.b	#'0', d0
	move.b	d0, -(a1)
	move.l	d5, d4
	bne.s	.digit
	tst.l	d3
	bpl.s	.done
	move.b	#'-', -(a1)
.done:
	rts

	; The protocol functions send_sync's bsr lands on.
	include	"inc/sidecart_functions.s"

text_ready:
	dc.b	"[ACC  ] - DevOps accessory ready", 13, 10, 0
text_menu_entry:
	dc.b	"  DevOps Runner", 0
; Alert lines stay within 30 characters, the TOS 1.x limit.
text_alert_head:
	dc.b	"[1][DevOps GEMDRIVE Runner|Waiting for commands.|Dir: ", 0
text_alert_last:
	dc.b	"|Last: ", 0
text_last_none:
	dc.b	"nothing yet", 0
text_last_run:
	dc.b	"run, exit ", 0
text_last_load:
	dc.b	"load, ", 0
text_last_exec:
	dc.b	"exec, exit ", 0
text_last_unload:
	dc.b	"unload, ", 0
text_last_cd:
	dc.b	"cd, ", 0
text_alert_tail:
	dc.b	"][  OK  ]", 0
text_run:
	dc.b	"[RUN  ] - Launching ", 0
text_load:
	dc.b	"[LOAD ] - Loading ", 0
text_load_result:
	dc.b	"[LOAD ] - result ", 0
text_exec:
	dc.b	"[EXEC ] - basepage ", 0
text_unload:
	dc.b	"[UNLD ] - basepage ", 0
text_unload_result:
	dc.b	"[UNLD ] - result ", 0
text_cd:
	dc.b	"[CD   ] - ", 0
text_exit_open:
	dc.b	"[EXIT ", 0
text_exit_close:
	dc.b	"] - ", 0
text_terminated:
	dc.b	" terminated"
text_crlf:
	dc.b	13, 10, 0
text_clear:
	dc.b	27, "E", 27, "e", 0		; clear screen, cursor on
text_cursor_off:
	dc.b	27, "f", 0
	even

	section	data,data

aes_pb:
	dc.l	control, global, int_in, int_out, addr_in, addr_out

acc_cwd:
	dc.b	$5C, 0				; backslash: the drive's root
	ds.b	126

	section	bss,bss

control:	ds.w	16
global:		ds.w	16
int_in:		ds.w	16
int_out:	ds.w	16
addr_in:	ds.l	4
addr_out:	ds.l	4

unacked_cmd:	ds.l	1
last_cmd:	ds.l	1
result:		ds.l	1
cache_setting:	ds.b	1	; the cache register as cache_off found it
		even
msg_buf:	ds.w	8
alert_buf:	ds.b	160
desk_xywh:	ds.w	4
saved_phys:	ds.l	1
saved_log:	ds.l	1
saved_rez:	ds.w	1
saved_palette:	ds.w	16
strip_len:	ds.l	1
strip_buf:	ds.b	STRIP_MAX
dec_buf:	ds.b	12

		ds.b	4096
acc_stack_top:
