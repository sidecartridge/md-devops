; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.

; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000

; Shared 64 KB region layout (must match rp/src/include/chandler.h).
;
;   $FA0000  CARTRIDGE			m68k header + code (max 10 KB)
;   $FA2800  CMD_MAGIC_SENTINEL_ADDR	4 B
;   $FA2804  RANDOM_TOKEN_ADDR		4 B
;   $FA2808  RANDOM_TOKEN_SEED_ADDR	4 B
;   $FA280C  reserved			4 B
;   $FA2810  SHARED_VARIABLES		240 B (60 x 4-byte slots)
;   $FA2900  APP_BUFFERS_ADDR	       ~46 KB free arena (TRANSTABLE etc.)
;   $FAE0C0  FRAMEBUFFER_ADDR		8000 B (320x200 mono, at the top)
;   $FAFFFF  end of region

CARTRIDGE_CODE_SIZE	equ $2800	; 10 KB max for cartridge header + code
SHARED_BLOCK_ADDR	equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)		; $FA2800
CMD_MAGIC_SENTINEL_ADDR	equ SHARED_BLOCK_ADDR				; $FA2800

FRAMEBUFFER_SIZE	equ 8000	; 8000 bytes of a 320x200 monochrome screen
FRAMEBUFFER_ADDR	equ (ROM4_ADDR + $10000 - FRAMEBUFFER_SIZE)	; $FAE040
APP_BUFFERS_ADDR	equ (SHARED_BLOCK_ADDR + $100)			; $FA2900
TRANSTABLE		equ APP_BUFFERS_ADDR				; high-res translation table

; Cartridge code layout (devops.ld; CARTRIDGE_CODE_SIZE = 10 KB):
;   $0000..$07FF  main.s     2 KB   boot + dispatch + terminal
;   $0800..$1BFF  gemdrive.s 5 KB   GEMDRIVE blob (relocated to RAM)
;   $1C00..$27FF  runner.s   3 KB   Runner foreground loop
; On mode-commit (rom_function or runner_function reached from the
; setup-menu polling), gemdrive_install copies GEMDRIVE_BLOB_SIZE
; bytes from GEMDRIVE_BLOB into a configurable RAM address (default
; screen_base - 16 KB) so the resident GEMDRIVE code can survive
; past cartridge teardown and be reached after _memtop has been
; lowered to protect the region. RUNNER_BLOB self-relocates at
; runner_entry into the same protected 16 KB region, just above
; GEMDRIVE_BLOB — see runner.s' RUNNER_ABOVE_GEMDRIVE_OFFSET.
GEMDRIVE_BLOB		equ (ROM4_ADDR + $800)			; $FA0800
GEMDRIVE_BLOB_SIZE	equ $1400				; 5 KB allocated by devops.ld
RUNNER_BLOB		equ (ROM4_ADDR + $1C00)			; $FA1C00
RUNNER_BLOB_SIZE	equ $C00				; 3 KB allocated by devops.ld

; Size of the address range the relocated blobs occupy that the
; supervisor stack must NOT sit inside. The full 16 KB protected
; region — equal to GEMDRIVE_DEFAULT_OFFSET_BYTES on the RP side.
; The danger zone is [gemdrive_reloc, gemdrive_reloc + RELOC_DANGER_ZONE_SIZE).
;
; Earlier revisions reserved the topmost 1 KB as tolerated slack,
; back when the check was incorrectly framed as "SP must be above
; the zone". With the corrected direction (SP below blob_start is
; always safe — pushes only move SP further down, away from the
; blobs), no slack is necessary; the full 16 KB is the actual
; relocation footprint and any SP landing inside it is a real
; collision.
RELOC_DANGER_ZONE_SIZE	equ $4000			; 16 KB

SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code
COLS_HIGH			equ 20		; 16 bit columns in the ST
ROWS_HIGH			equ 200		; 200 rows in the ST
BYTES_ROW_HIGH		equ 80		; 80 bytes per row in the ST
PRE_RESET_WAIT		equ $FFFFF

; If 1, the display will not use the framebuffer and will write directly to the
; display memory. This is useful to reduce the memory usage in the rp2040
; When not using the framebuffer, the endianness swap must be done in the atari ST
DISPLAY_BYPASS_FRAMEBUFFER 	equ 1

CMD_NOP				equ 0		; No operation command
CMD_RESET			equ 1		; Reset command
CMD_BOOT_GEM		equ 2		; Boot GEM command
CMD_TERMINAL		equ 3		; Terminal command
CMD_START			equ 4		; Hand control to GEMDRIVE (rom_function)
CMD_START_RUNNER	equ 5		; Hand control to the Runner (runner_function)

; GEMDRIVE app namespace. Wire-format compatible with md-drives-emulator:
; APP_GEMDRVEMUL keeps the same value; CMD_GEMDRVEMUL_* values that exist
; in the source (GEMDRVEMUL_RESET = 0, _SAVE_VECTORS = 1, etc.) will be
; reused unchanged when S2 lands. CMD_GEMDRIVE_HELLO is a new S1-only
; bootstrap command; $50 is unused in the source's allocation table.
APP_GEMDRVEMUL			equ $0400
CMD_GEMDRIVE_HELLO		equ ($50 + APP_GEMDRVEMUL)
; Diagnostic: m68k publishes the value it observes at $436 immediately
; after the patch so the RP can confirm the write actually landed.
CMD_GEMDRIVE_VERIFY_MEMTOP	equ ($52 + APP_GEMDRVEMUL)

; Indexed shared variable slots used by GEMDRIVE. Slots 0..2 are framework
; (chandler.h: HARDWARE_TYPE / SVERSION / BUFFER_TYPE), 3..9 reserved for
; future framework use. GEMDRIVE owns 10..11; further GEMDRIVE state will
; claim 12..15 in later stories.
SHARED_VAR_GEMDRIVE_RELOC_ADDR	equ 10		; RP-published reloc address
SHARED_VAR_GEMDRIVE_MEMTOP	equ 11		; RP-published memtop value

_conterm			equ $484	; Conterm device number


; Constants needed for the commands
RANDOM_TOKEN_ADDR:        equ (CMD_MAGIC_SENTINEL_ADDR + 4)  ; $FA2804
RANDOM_TOKEN_SEED_ADDR:   equ (RANDOM_TOKEN_ADDR + 4)        ; $FA2808
; $FA280C: 4-byte slot reserved for future framework use. chandler_init
; zeroes it at boot; apps must not write here.
RESERVED_SLOT_ADDR:       equ (RANDOM_TOKEN_SEED_ADDR + 4)   ; $FA280C
RANDOM_TOKEN_POST_WAIT:   equ $1                             ; Wait cycles after the RNG is ready
COMMAND_TIMEOUT           equ $0000FFFF                      ; Timeout for the command
COMMAND_WRITE_TIMEOUT     equ COMMAND_TIMEOUT                ; Timeout for write commands

SHARED_VARIABLES:         equ (RESERVED_SLOT_ADDR + 4)       ; $FA2810 (60 indexed 4-byte slots)

ROMCMD_START_ADDR:        equ $FB0000					  ; We are going to use ROM3 address
CMD_MAGIC_NUMBER    	  equ ($ABCD) 					  ; Magic number header to identify a command
CMD_RETRIES_COUNT	  	  equ 3							  ; Number of retries for the command
CMD_SET_SHARED_VAR		  equ 1							  ; This is a fake command to set the shared variables
														  ; Used to store the system settings
; App commands for the terminal
APP_TERMINAL 				equ $0 ; The terminal app

; App terminal commands
APP_TERMINAL_START   		equ $0 ; Start terminal command
APP_TERMINAL_KEYSTROKE 		equ $1 ; Keystroke command

_dskbufp                equ $4c6                            ; Address of the disk buffer pointer    


	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Vsync wait
vsync_wait          macro
					move.w #37,-(sp)
					trap #14
					addq.l #2,sp
                    endm    

; XBIOS GetRez
; Return the current screen resolution in D0
get_rez				macro
					move.w #4,-(sp)
					trap #14
					addq.l #2,sp
					endm

; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

; Check the left or right shift key. If pressed, exit.
check_shift_keys	macro
					move.w #-1, -(sp)			; Read all key status
					move.w #$b, -(sp)			; BIOS Get shift key status
					trap #13
					addq.l #4,sp

					btst #1,d0					; Left shift skip and boot GEM
					bne boot_gem

					btst #0,d0					; Right shift skip and boot GEM
					bne boot_gem

					endm

; Check the keys pressed
check_keys			macro

					gemdos	Cconis,2		; Check if a key is pressed
					tst.l d0
					beq .\@no_key

					gemdos	Cnecin,2		; Read the key pressed

					cmp.b #27, d0		; Check if the key is ESC
					beq .\@esc_key	; If it is, send terminal command

					move.l d0, d3
					send_sync APP_TERMINAL_KEYSTROKE, 4

					bra .\@no_key
.\@esc_key:
					send_sync APP_TERMINAL_START, 0

.\@no_key:

					endm

check_commands		macro
					move.l CMD_MAGIC_SENTINEL_ADDR, d6	; Store in the D6 register the remote command value
					cmp.l #CMD_TERMINAL, d6		; Check if the command is a terminal command
					bne.s .\@check_reset

					; Check the keys for the terminal emulation
					check_keys
					bra .\@bypass
.\@check_reset:
					cmp.l #CMD_RESET, d6		; Check if the command is a reset
					beq .reset					; If it is, reset the computer
					cmp.l #CMD_BOOT_GEM, d6		; Check if the command is to boot GEM
					beq boot_gem				; If it is, boot GEM
					cmp.l #CMD_START, d6		; Check if the command hands over to USERFW
					beq rom_function			; If it is, jump to the user firmware dispatcher
					cmp.l #CMD_START_RUNNER, d6	; Check if the command hands over to the Runner
					beq runner_function			; If it is, jump to the Runner blob entry

					; If we are here, the command is a NOP
					; If the command is a NOP, check the shift keys to bypass the command
					; check_shift_keys
					check_keys
.\@bypass:
					endm

	section

;Rom cartridge
; The cartridge image (header + code below) MUST fit in
; CARTRIDGE_CODE_SIZE = $2800 (10 KB). The hard limit is enforced by
; target/atarist/build.sh after vlink emits BOOT.BIN; any direct vasm /
; vlink invocation that bypasses the build script is unchecked, so keep
; an eye on BOOT.BIN's size when iterating outside ./build.sh.

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "TERM",0
    even

pre_auto:
; Boot-time handshake with the RP only — sends screen_base, reads back
; the effective reloc / memtop. The actual GEMDRIVE blob copy + Setexc
; install is deferred to gemdrive_install, called from rom_function /
; runner_function once the user commits a mode at the setup menu. That
; way a bad reloc destination (overlap with stack, weird screen-base
; placement) cannot prevent the menu from painting — the user can
; recover via the [R] menu option without a watchdog reboot.
	bsr gemdrive_handshake

; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; We assume the screen memory address is in D0 after the get_screen_base call
	move.l d0, a6				; Save the screen memory address in A6

; Enable bconin to return shift key status
	or.b #%1000, _conterm.w

; Get the resolution of the screen
	get_rez
	cmp.w #2, d0				; Check if the resolution is 640x400 (high resolution)
	beq .print_loop_high		; If it is, print the message in high resolution

.print_loop_low:
	vsync_wait

; We must move from the cartridge ROM to the screen memory to display the messages
	move.l a6, a0				; Set the screen memory address in a0
	move.l #FRAMEBUFFER_ADDR, a1			; Set the cartridge ROM address in a1
	move.l #((FRAMEBUFFER_SIZE / 2) -1), d0			; Set the number of words to copy
.copy_screen_low:
	move.w (a1)+ , d1			; Copy a word from the cartridge ROM
	ifne DISPLAY_BYPASS_FRAMEBUFFER == 1
	rol.w #8, d1				; swap high and low bytes
	endif
	move.w d1, d2				; Copy the word to d2
	swap d2						; Swap the bytes
	move.w d1, d2				; Copy the word to d2
	move.l d2, (a0)+			; Copy the word to the screen memory
	move.l d2, (a0)+			; Copy the word to the screen memory
	dbf d0, .copy_screen_low    ; Loop until all the message is copied

; Check the different commands and the keyboard
	check_commands

	bra .print_loop_low		; Continue printing the message

.print_loop_high:
	vsync_wait

; We must move from the cartridge ROM to the screen memory to display the messages
	move.l a6, a1				; Set the screen memory address in a1
	move.l a6, a2
	lea BYTES_ROW_HIGH(a2), a2	; Move to the next line in the screen
	move.l #FRAMEBUFFER_ADDR, a0		; Set the cartridge ROM address in a0
	move.l #TRANSTABLE, a3		; Set the translation table in a3
	move.l #(ROWS_HIGH -1), d0	; Set the number of rows to copy - 1
.copy_screen_row_high:
	move.l #(COLS_HIGH -1), d1	; Set the number of columns to copy - 1 
.copy_screen_col_high:
	move.w (a0)+ , d2			; Copy a word from the cartridge ROM

	ifne DISPLAY_BYPASS_FRAMEBUFFER == 1
	rol.w #8, d2				; swap high and low bytes
	endif

	move.w d2, d3				; Copy the word to d3
	and.w #$FF00, d3			; Mask the high byte
	lsr.w #7, d3				; Shift the high byte 7 bits to the right
	move.w (a3, d3.w), d4		; Translate the high byte
	swap d4						; Swap the words

	and.w #$00FF, d2			; Mask the low byte
	add.w d2, d2				; Double the low byte
	move.w (a3, d2.w), d4		; Translate the low byte

	move.l d4, (a1)+			; Copy the word to the screen memory
	move.l d4, (a2)+			; Copy the word to the screen memory

	dbf d1, .copy_screen_col_high   ; Loop until all the message is copied

	lea BYTES_ROW_HIGH(a1), a1	; Move to the next line in the screen
	lea BYTES_ROW_HIGH(a2), a2	; Move to the next line in the screen

	dbf d0, .copy_screen_row_high   ; Loop until all the message is copied

; Check the different commands and the keyboard
	check_commands

	bra .print_loop_high		; Continue printing the message
	
.reset:
    move.l #PRE_RESET_WAIT, d6
.wait_me:
    subq.l #1, d6           ; Decrement the outer loop
    bne.s .wait_me          ; Wait for the timeout

	clr.l $420.w			; Invalidate memory system variables
	clr.l $43A.w
	clr.l $51A.w
	move.l $4.w, a0			; Now we can safely jump to the reset vector
	jmp (a0)
	nop

boot_gem:
	; If we get here, continue loading GEM
    rts

; Dispatcher invoked on CMD_START from the sentinel poll in
; check_commands — the user picked [G]/[F] in the setup terminal
; (or the autoboot countdown elapsed onto the GEMDRIVE-only path).
; First call gemdrive_install (deferred copy + Setexc trap-#1 hook),
; then jump to the diagnostic entry at offset +4 of the relocated
; blob (entry table: bra.w install at +0, bra.w diagnostic at +4).
; The diagnostic prints the GEMDRIVE banner; install_entry was
; already run by gemdrive_install, so we route to +4 and not +0.
rom_function:
    jsr gemdrive_install
    jmp GEMDRIVE_BLOB+4

; Dispatcher invoked on CMD_START_RUNNER from the sentinel poll in
; check_commands — the user pressed [U] in the setup terminal.
; Install the deferred GEMDRIVE blob + trap-#1 hook so programs
; the Runner Pexec's see the emulated drive, then hand control to
; runner_entry. RUNNER_BLOB self-relocates from cartridge ROM into
; the protected 16 KB region (above the relocated GEMDRIVE blob)
; via runner.s' relocation harness, and never returns.
runner_function:
    jsr gemdrive_install
    jmp RUNNER_BLOB

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
;
; Export send_sync_command_to_sidecart so runner.s' send_sync macro
; (which lives in a separate object) can `bsr` to this single copy
; via vlink. gemdrive.s has its OWN private copy because it's
; relocated to RAM and the BSR must land in the relocated blob; the
; Runner runs in place from cartridge ROM, so a cross-module BSR
; back into main.o resolves fine.
    xdef send_sync_command_to_sidecart
    include "inc/sidecart_functions.s"


end_rom_code:

	even

; -----------------------------------------------------------------------
; gemdrive_handshake — boot-time HELLO round-trip (called once from
; pre_auto, before the menu paints).
; -----------------------------------------------------------------------
; Reads screen_base via XBIOS Logbase, sends CMD_GEMDRIVE_HELLO, and
; lets the RP-side gemdrive_command_cb apply aconfig overrides
; (GEMDRIVE_RELOC_ADDR / GEMDRIVE_MEMTOP) or fall back to the default
; screen_base - 16 KB. By the time send_sync's random-token ack
; returns, the effective reloc + memtop have been written into shared
; variable slots SHARED_VAR_GEMDRIVE_RELOC_ADDR and
; SHARED_VAR_GEMDRIVE_MEMTOP. The setup-menu rendering reads those
; slots so the user sees the resolved [R]eloc addr immediately.
;
; The actual GEMDRIVE blob copy + _memtop patch + Setexc install are
; NOT done here — they're deferred to gemdrive_install (called from
; rom_function / runner_function on mode commit) so the menu remains
; reachable even if the chosen reloc destination is unsafe.
;
; This routine lives outside start_rom_code..end_rom_code so it is
; not copied with the print-loop relocation; it executes once from
; cartridge ROM and is never re-entered.
gemdrive_handshake:
	movem.l d0-d7/a0-a3, -(sp)

	; Logbase (XBIOS fn 2) → d0.l = screen base
	move.w #2, -(sp)
	trap #14
	addq.l #2, sp

	; Publish screen_base, RP applies overrides + writes back
	move.l d0, d3
	send_sync CMD_GEMDRIVE_HELLO, 4

	movem.l (sp)+, d0-d7/a0-a3
	rts

; gemdrive_install — deferred GEMDRIVE relocation + trap-#1 install.
; -----------------------------------------------------------------------
; Called from rom_function and runner_function once the user has
; committed a mode at the setup menu (or the autoboot countdown
; elapsed). Reads the effective reloc + memtop that gemdrive_handshake
; published into shared variables, copies the GEMDRIVE blob to the
; chosen RAM destination, lowers _memtop so TOS won't allocate over
; it, and calls into install_entry (offset 0 of the relocated blob)
; to wire up the GEMDOS trap-#1 hook.
;
; Lives outside start_rom_code..end_rom_code, so the print-loop
; relocation doesn't carry a stale copy. The dispatchers in the
; relocated print-loop reach this routine via absolute-long jsr.
;
; Called exactly once per ST cold reset: each user mode-commit fires
; one of the two dispatchers, which calls this routine, after which
; either the diagnostic (rom_function) or runner_entry
; (runner_function) takes over and never returns to the menu.
gemdrive_install:
	movem.l d0-d7/a0-a3, -(sp)

	; Read the effective reloc and memtop the RP wrote during the
	; boot-time handshake.
	move.l SHARED_VARIABLES+(SHARED_VAR_GEMDRIVE_RELOC_ADDR*4), d0
	move.l SHARED_VARIABLES+(SHARED_VAR_GEMDRIVE_MEMTOP*4), d1

	; --- Stack-overlap safety check ---
	; Abort iff SP currently sits *inside* the relocation danger
	; zone [gemdrive_reloc, gemdrive_reloc + RELOC_DANGER_ZONE_SIZE).
	;
	; If SP < gemdrive_reloc, the stack is BELOW the blobs — pushes
	; only lower SP further so it can never grow into the blobs.
	; Safe regardless of how deep the call chain gets. This is the
	; common TOS layout: supervisor stack lives in low memory
	; (~0x1000–0x6000), blobs land just below screen_base.
	;
	; If SP >= gemdrive_reloc + RELOC_DANGER_ZONE_SIZE, the stack
	; is above the zone — pushes lower SP but the zone size is the
	; full 16 KB protected region, so SP entering the zone via
	; pushes from above would require a single push of more than
	; the entire zone, which doesn't happen in practice.
	;
	; Only the in-zone case is actually dangerous, so that's what
	; we abort on.
	;
	; Single test covers runner_entry's later self-relocation too:
	; runner_entry runs after this routine with an essentially-
	; identical SP and lands inside this same zone.
	move.l d0, d3				; d3 = blob_start
	move.l sp, d4
	cmp.l d3, d4
	bcs.w .gd_stack_safe			; sp < blob_start → safe (below)
	add.l #RELOC_DANGER_ZONE_SIZE, d3	; d3 = blob_start + 16 KB
	cmp.l d3, d4
	bcs.w .gd_stack_overlap			; blob_start ≤ sp < zone_end → ABORT
.gd_stack_safe:

	; Copy GEMDRIVE_BLOB from cartridge ROM to the user-chosen RAM dst.
	move.l d0, a1
	move.l #GEMDRIVE_BLOB, a0
	move.l #GEMDRIVE_BLOB_SIZE, d2
	lsr.w #2, d2
	subq #1, d2
.gd_install_copy_loop:
	move.l (a0)+, (a1)+
	dbf d2, .gd_install_copy_loop

	; Lower _memtop so TOS won't allocate over the resident blob on
	; subsequent Pexec calls. We're still inside CA_INIT bit 27
	; (pre_auto hasn't returned to TOS yet for the GEMDRIVE-only path,
	; or the Runner is about to take over and never return), so the
	; patch lands before any TPA computations.
	move.l d1, memtop.w

	; Call into the relocated install_entry (offset 0 of the blob:
	; entry table is bra.w install at +0, bra.w diagnostic at +4 —
	; see gemdrive.s). install_entry runs Setexc + sends
	; CMD_SAVE_VECTORS to the RP, then rts back here. d0 still
	; holds the reloc address.
	move.l d0, a1
	jsr (a1)

	movem.l (sp)+, d0-d7/a0-a3
	rts

.gd_stack_overlap:
	; Print the warning + halt. We don't restore registers / rts
	; because the caller would otherwise jmp into a non-installed
	; blob and crash worse. Halt loop is recoverable via SELECT /
	; power-cycle, which brings the user back to the menu where
	; [R] reloc addr can be raised.
	print	gd_overlap_msg
.gd_stack_overlap_halt:
	bra.s	.gd_stack_overlap_halt

gd_overlap_msg:
	dc.b	13,10,'Reloc/stack overlap.',13,10
	dc.b	'Raise [R] in setup menu, reset.',13,10,0
	even

end_pre_auto:
	even
	dc.l $DEADFFFF
	dc.l 0