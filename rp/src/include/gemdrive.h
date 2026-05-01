/**
 * File: gemdrive.h
 * Description: GEMDRIVE — Epic 01. S1 surface: relocation handshake only;
 * GEMDOS hooks land in S2+.
 */

#ifndef GEMDRIVE_H
#define GEMDRIVE_H

#include "chandler.h"
#include "tprotocol.h"

// Wire-format identifiers, kept identical to md-drives-emulator so the
// m68k↔RP protocol matches once full GEMDOS hooks land.
#define GEMDRIVE_APP 0x0400  // APP_GEMDRVEMUL — MSB of the 16-bit cmd id

// CMD_GEMDRIVE_HELLO is new in this app (S1 bootstrap). $50 is unused
// in the source's allocation table.
#define GEMDRIVE_CMD_HELLO (0x50 + GEMDRIVE_APP)

// Diagnostic: m68k sends what it observes at $436 immediately after
// patching _memtop. The RP logs it so we can confirm the write landed
// (e.g. that we're not silently writing to the wrong address).
#define GEMDRIVE_CMD_VERIFY_MEMTOP (0x52 + GEMDRIVE_APP)

// Wire-format identical to md-drives-emulator. RESET_GEM clears any
// per-session state held by the RP (none in S2 — placeholder for S3+).
// SAVE_VECTORS records the previous GEMDOS handler address and the
// blob's old_handler cell address; the RP uses both to verify the
// install ran and (in later stories) to chain back to the original
// handler when the RP itself simulates a GEMDOS call.
#define GEMDRIVE_CMD_RESET_GEM (0x00 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_SAVE_VECTORS (0x01 + GEMDRIVE_APP)

// S3 wire format (kept identical to md-drives-emulator).
#define GEMDRIVE_CMD_REENTRY_LOCK (0x03 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_REENTRY_UNLOCK (0x04 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DFREE_CALL (0x36 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DCREATE_CALL (0x39 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DDELETE_CALL (0x3A + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DSETPATH_CALL (0x3B + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FCREATE_CALL (0x3C + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FOPEN_CALL (0x3D + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FCLOSE_CALL (0x3E + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FDELETE_CALL (0x41 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FSEEK_CALL (0x42 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FATTRIB_CALL (0x43 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DGETPATH_CALL (0x47 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_PEXEC_CALL (0x4B + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FSETDTA_CALL (0x1A + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FSFIRST_CALL (0x4E + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FSNEXT_CALL (0x4F + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FRENAME_CALL (0x56 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_FDATETIME_CALL (0x57 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_READ_BUFF_CALL (0x81 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_SAVE_BASEPAGE (0x83 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_SAVE_EXEC_HEADER (0x84 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_WRITE_BUFF_CALL (0x88 + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DTA_EXIST_CALL (0x8A + GEMDRIVE_APP)
#define GEMDRIVE_CMD_DTA_RELEASE_CALL (0x8B + GEMDRIVE_APP)

// Indexed shared variable slots written by gemdrive_command_cb. Slots
// 0..2 are framework (chandler.h: HARDWARE_TYPE / SVERSION / BUFFER_TYPE),
// 3..9 reserved.
#define GEMDRIVE_SVAR_RELOC_ADDR 10
#define GEMDRIVE_SVAR_MEMTOP 11
#define GEMDRIVE_SVAR_DRIVE_NUMBER 12  // 0=A, 1=B, 2=C, ...
#define GEMDRIVE_SVAR_DRIVE_LETTER 13  // letter in low byte; m68k reads at slot+3
#define GEMDRIVE_SVAR_REENTRY_TRAP 14  // 0=unlocked, non-zero=locked
#define GEMDRIVE_SVAR_FIRST_FILE_DESCRIPTOR 15  // base file handle (Phase 2)

// Per-handler scratch state lives in the cartridge APP_FREE region
// (currently $FA2B00+; CHANDLER_APP_FREE_OFFSET in chandler.h is the
// authoritative offset within the shared region) so the m68k can read
// it via absolute addressing without burning shared-variable slots.
// Offsets below are relative to the APP_FREE base.
#define GEMDRIVE_DEFAULT_PATH_OFFSET 0x000
#define GEMDRIVE_DEFAULT_PATH_LEN 128
#define GEMDRIVE_DFREE_STATUS_OFFSET 0x080
#define GEMDRIVE_DFREE_STRUCT_OFFSET 0x084  // 16 bytes (4 × u32)

// Phase 2 state extending APP_FREE.
#define GEMDRIVE_SET_DPATH_STATUS_OFFSET 0x090  // 4 bytes
#define GEMDRIVE_FOPEN_HANDLE_OFFSET 0x094     // 4 bytes (signed: handle / err)
#define GEMDRIVE_FCLOSE_STATUS_OFFSET 0x098    // 4 bytes
#define GEMDRIVE_FSEEK_STATUS_OFFSET 0x09C     // 4 bytes (new abs pos / err)
#define GEMDRIVE_READ_BYTES_OFFSET 0x0A0       // 4 bytes (signed: count / err)
#define GEMDRIVE_READ_BUFFER_OFFSET 0x0A4      // 4096 bytes
#define GEMDRIVE_READ_BUFFER_SIZE 4096
#define GEMDRIVE_DTA_F_FOUND_OFFSET 0x10A4     // 4 bytes (0 = found)
#define GEMDRIVE_DTA_TRANSFER_OFFSET 0x10A8    // 44 bytes (DTA_SIZE)
#define GEMDRIVE_DTA_TRANSFER_SIZE 44

// S4 write-side state. All status words are signed GEMDOS error codes
// (0 = ok, negative = error). FCREATE_HANDLE returns a new file handle
// in the GEMDRIVE range or a negative error.
#define GEMDRIVE_DCREATE_STATUS_OFFSET 0x10D8   // 4 bytes
#define GEMDRIVE_DDELETE_STATUS_OFFSET 0x10DC   // 4 bytes
#define GEMDRIVE_FCREATE_HANDLE_OFFSET 0x10E0   // 4 bytes (handle / err)
#define GEMDRIVE_FDELETE_STATUS_OFFSET 0x10E4   // 4 bytes
#define GEMDRIVE_FATTRIB_STATUS_OFFSET 0x10E8   // 4 bytes (new attrib / err)
#define GEMDRIVE_FRENAME_STATUS_OFFSET 0x10EC   // 4 bytes
#define GEMDRIVE_WRITE_BYTES_OFFSET 0x10F0      // 4 bytes (bytes written)
#define GEMDRIVE_WRITE_BUFFER_OFFSET 0x10F4     // 1024 bytes
#define GEMDRIVE_WRITE_BUFFER_SIZE 1024

// S5 Pexec / DTA state.
#define GEMDRIVE_PEXEC_MODE_OFFSET 0x14F4       // 4 bytes (mode word at +2)
#define GEMDRIVE_PEXEC_STACK_ADDR_OFFSET 0x14F8 // 4 bytes
#define GEMDRIVE_PEXEC_FNAME_OFFSET 0x14FC      // 4 bytes
#define GEMDRIVE_PEXEC_CMDLINE_OFFSET 0x1500    // 4 bytes
#define GEMDRIVE_PEXEC_ENVSTR_OFFSET 0x1504     // 4 bytes
// EXEC_HEADER is 28 bytes; EXEC_PD is the FULL 256-byte basepage that
// m68k ships via CMD_SAVE_BASEPAGE. Order matters — putting EXEC_PD
// FIRST and only 4 bytes overlapped EXEC_HEADER and silently corrupted
// the saved tsize/dsize/ssize. Keep EXEC_PD last so basepage memcpys
// can't trample anything sitting after it.
#define GEMDRIVE_EXEC_HEADER_OFFSET 0x150C      // 32 bytes (PRG header)
#define GEMDRIVE_DTA_EXIST_OFFSET 0x152C        // 4 bytes
#define GEMDRIVE_DTA_RELEASE_OFFSET 0x1530      // 4 bytes
#define GEMDRIVE_EXEC_PD_OFFSET 0x1534          // 256 bytes (basepage)
#define GEMDRIVE_FDATETIME_DATE_OFFSET 0x1620   // 4 bytes (DOS date)
#define GEMDRIVE_FDATETIME_TIME_OFFSET 0x1624   // 4 bytes (DOS time)
#define GEMDRIVE_FDATETIME_STATUS_OFFSET 0x1628 // 4 bytes

// Shared variable for the PE_GO restoration trampoline (TOS 1.00/1.02
// hack — see source's gemdrive.s for the rationale).
#define GEMDRIVE_SVAR_PEXEC_RESTORE 16

// File-handle window. Anything ≥ FIRST_FD belongs to GEMDRIVE; below
// passes through to the original GEMDOS handler. Published in shared
// var slot 15 so the m68k's detect_emulated_file_handler macro can
// compare against it.
#define GEMDRIVE_FIRST_FD 64
#define GEMDRIVE_MAX_OPEN_FILES 8
// Source uses a malloc'd linked list (uncapped); we approximate with a
// fixed table + LRU eviction. 16 covers typical desktop sub-folder
// browsing, where the desktop holds 5–10 live DTAs while drilling.
#define GEMDRIVE_MAX_DTAS 16

// The default reloc / memtop = screen_base - GEMDRIVE_DEFAULT_OFFSET_BYTES.
// Mirrors GEMDRIVE_BLOB_SIZE on the m68k side and matches the user's
// stated requirement ("8 KB below the screen memory address").
#define GEMDRIVE_DEFAULT_OFFSET_BYTES 0x2000

// Initialize GEMDRIVE module: register the command callback. Must be
// called after chandler_init() and before the m68k cartridge issues
// CMD_GEMDRIVE_HELLO.
void gemdrive_init(void);

// chandler callback. Public so emul.c can register it via chandler_addCB.
void gemdrive_command_cb(TransmissionProtocol *protocol, uint16_t *payload);

#endif  // GEMDRIVE_H
