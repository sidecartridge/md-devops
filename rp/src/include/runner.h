/**
 * File: runner.h
 * Author: Diego Parrilla Santamaría
 * Date: April 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Runner app shared region offsets + protocol
 *              constants. Mirrored on the m68k side in
 *              target/atarist/src/runner.s and main.s. Epic 03.
 */

#ifndef RUNNER_H
#define RUNNER_H

#include "chandler.h"

// Runner sub-region inside APP_FREE. Positioned past GEMDRIVE's
// existing allocation (highest GEMDRIVE offset is 0x162C; we leave
// ~512 B of slack before this base).
#define RUNNER_BASE_OFFSET 0x1800

#define RUNNER_PATH_OFFSET (RUNNER_BASE_OFFSET + 0x000)         // 128 B
#define RUNNER_PATH_LEN 128
#define RUNNER_CMDLINE_OFFSET (RUNNER_BASE_OFFSET + 0x080)      // 128 B
#define RUNNER_CMDLINE_LEN 128
#define RUNNER_LAST_STATUS_OFFSET (RUNNER_BASE_OFFSET + 0x100)  // 4 B
#define RUNNER_DONE_FLAG_OFFSET (RUNNER_BASE_OFFSET + 0x104)    // 4 B
#define RUNNER_CWD_OFFSET (RUNNER_BASE_OFFSET + 0x108)          // 128 B
#define RUNNER_CWD_LEN 128
#define RUNNER_HELLO_OFFSET (RUNNER_BASE_OFFSET + 0x188)        // 4 B
#define RUNNER_PROTO_VER_OFFSET (RUNNER_BASE_OFFSET + 0x18C)    // 4 B
// Stateless target rez for RUNNER_CMD_RES (u16 in low half;
// 0=low, 1=med). Stored as 4 B for word-aligned access.
#define RUNNER_REZ_OFFSET (RUNNER_BASE_OFFSET + 0x190)          // 4 B
// Pexec-load basepage cache (Epic 06 / S5+S6). The m68k stashes
// the Pexec(3) basepage pointer here on RUNNER_CMD_LOAD, then
// reads it back on RUNNER_CMD_EXEC for the Pexec(4) call. The
// RP also tracks the basepage in its own state (mirrored from
// the DONE_LOAD payload) for the strict-refuse semantics.
#define RUNNER_BASEPAGE_OFFSET (RUNNER_BASE_OFFSET + 0x194)     // 4 B

// Magic the m68k Runner publishes at boot so the RP can detect that
// Runner mode is active. ASCII 'RNV1' big-endian. Reserved for
// future protocol-version handshake; S1 detects active state RP-side
// (cmdRunner flips emul_isRunnerActive()).
#define RUNNER_HELLO_MAGIC 0x524E5631u

// Protocol version: high u16 = min, low u16 = max. v1.
#define RUNNER_PROTO_VERSION 0x00010001u

// Runner command IDs. Distinct from main.s' framework values
// (CMD_NOP=0, CMD_RESET=1, CMD_BOOT_GEM=2, CMD_TERMINAL=3,
// CMD_START=4, CMD_START_RUNNER=5) by living in the $0500 namespace.
// The RP writes one of these into the cartridge sentinel
// (CHANDLER_CMD_SENTINEL_OFFSET) to dispatch a command; the Runner's
// poll loop reads and acts on it.
#define APP_RUNNER 0x0500
// Note: APP_RUNNER + 0x01 was the foreground RUNNER_CMD_RESET; retired
// in Epic 04 / S5 in favour of RUNNER_ADV_CMD_RESET (VBL-driven). Slot
// kept reserved so future stories don't reuse it accidentally.
#define RUNNER_CMD_EXECUTE (APP_RUNNER + 0x02)  // Pexec mode 0
#define RUNNER_CMD_CD (APP_RUNNER + 0x03)       // Dsetpath
#define RUNNER_CMD_RES (APP_RUNNER + 0x04)      // XBIOS Setscreen
#define RUNNER_CMD_MEMINFO (APP_RUNNER + 0x05)  // system memory snapshot
#define RUNNER_CMD_LOAD (APP_RUNNER + 0x06)     // Pexec mode 3 (Epic 06 / S5)
#define RUNNER_CMD_EXEC (APP_RUNNER + 0x07)     // Pexec mode 4 (Epic 06 / S6)
#define RUNNER_CMD_UNLOAD (APP_RUNNER + 0x08)   // Mfree(basepage) (Epic 06 / S7)

// Epic 04 — Advanced Runner. Commands in this range are dispatched
// by the m68k's VBL ISR (installed at $70 by runner_post_reloc) so
// they keep working when the foreground poll loop is wedged. The
// existing $05xx foreground range is unaffected — the poll loop's
// cmp.l cascade only matches $05xx codes, so $06xx values fall
// through to the VBL handler.
#define APP_RUNNER_VBL 0x0600
#define RUNNER_ADV_CMD_RESET (APP_RUNNER_VBL + 0x01)    // forced cold reset
// 0x02 reserved.
#define RUNNER_ADV_CMD_MEMINFO (APP_RUNNER_VBL + 0x03)  // meminfo from inside the ISR
#define RUNNER_ADV_CMD_JUMP (APP_RUNNER_VBL + 0x04)     // rte to user-supplied address
#define RUNNER_ADV_CMD_LOAD_CHUNK \
  (APP_RUNNER_VBL + 0x05)  // copy 8 KB chunk from APP_FREE → m68k RAM

// m68k -> RP report commands for the VBL command range. $0680+ to
// keep the receiver path unambiguous (RP -> m68k uses $06xx without
// the $80 bit set).
#define APP_RUNNER_VBL_DONE 0x0680
#define RUNNER_ADV_CMD_DONE_JUMP \
  (APP_RUNNER_VBL_DONE + 0x00)  // no payload — RP clears sentinel
#define RUNNER_ADV_CMD_DONE_LOAD_CHUNK \
  (APP_RUNNER_VBL_DONE + 0x01)  // no payload — RP clears + advances

// Shared-variable slot 16 — Advanced Runner hook vector address.
// RP publishes the resolved address ($70 for VBL, $400 for
// etv_timer) at HELLO time based on the ACONFIG_PARAM_ADV_HOOK_VECTOR
// aconfig setting; m68k's runner_post_reloc reads it to know where
// to install adv_hook_handler. Slots 0..15 are claimed by chandler
// framework + GEMDRIVE; 16 is the first runner-claimed slot.
#define RUNNER_SVAR_ADV_HOOK_VECTOR 16

// Hook vector identifiers used in the HELLO payload byte and in the
// JSON envelope for GET /api/v1/runner/adv. 0xFF reserved for
// "unknown" (m68k didn't report — older firmware or no HELLO yet).
#define RUNNER_HOOK_VECTOR_VBL 0
#define RUNNER_HOOK_VECTOR_ETV_TIMER 1
#define RUNNER_HOOK_VECTOR_UNKNOWN 0xFF

// m68k -> RP report commands (sent via send_sync from the Runner).
// High bit set so they don't collide with the RP -> m68k sentinel
// command IDs above.
#define RUNNER_CMD_DONE_EXECUTE (APP_RUNNER + 0x82)  // payload: i32 exit code
#define RUNNER_CMD_DONE_CD (APP_RUNNER + 0x83)       // payload: i32 GEMDOS errno
// Sent unconditionally from runner_entry every time the Runner
// (re)enters its poll loop — covers physical reset, cold reset, and
// menu re-entry. RP clears the session-transient state (busy, cwd
// mirror) so a status query right after a reset reflects "idle".
#define RUNNER_CMD_DONE_HELLO (APP_RUNNER + 0x84)    // no payload
#define RUNNER_CMD_DONE_RES (APP_RUNNER + 0x85)      // payload: i32 errno
#define RUNNER_CMD_DONE_MEMINFO (APP_RUNNER + 0x86)  // payload: 24-byte struct
#define RUNNER_CMD_DONE_LOAD (APP_RUNNER + 0x87)     // payload: i32 (>0 basepage; <0 -errno)
#define RUNNER_CMD_DONE_EXEC (APP_RUNNER + 0x88)     // payload: i32 exit code
#define RUNNER_CMD_DONE_UNLOAD (APP_RUNNER + 0x89)   // payload: i32 Mfree result (0 OK, <0 errno)

// RP-side state machine mirror. cmdRunner sets ACTIVE; per-command
// handlers update last_command. Used by GET /api/v1/runner.
typedef enum {
  RUNNER_LAST_NONE = 0,
  RUNNER_LAST_RESET = 1,
  RUNNER_LAST_EXECUTE = 2,
  RUNNER_LAST_CD = 3,
  RUNNER_LAST_RES = 4,
  RUNNER_LAST_MEMINFO = 5,
  RUNNER_LAST_JUMP = 6,
  RUNNER_LAST_LOAD = 7,         // adv-load (raw RAM upload, Epic 04 / S8)
  RUNNER_LAST_PEXEC_LOAD = 8,    // runner load (Pexec(3), Epic 06 / S5)
  RUNNER_LAST_PEXEC_EXEC = 9,    // runner exec (Pexec(4), Epic 06 / S6)
  RUNNER_LAST_PEXEC_UNLOAD = 10, // runner unload (Mfree, Epic 06 / S7)
} runner_last_command_t;

// Snapshot returned by RUNNER_CMD_MEMINFO. Mirrors the 24-byte
// struct the m68k builds in runner.s' runner_meminfo handler.
typedef struct {
  uint32_t membot;     // _membot   ($432)
  uint32_t memtop;     // _memtop   ($436)
  uint32_t phystop;    // _phystop  ($42E)
  uint32_t screenmem;  // _v_bas_ad ($44E) logical screen base
  uint32_t basepage;   // _run      ($4F2) — 0 on TOS < 1.04
  uint16_t bank0_kb;   // 0 = unknown / unrecognised MMU config
  uint16_t bank1_kb;
} runner_meminfo_t;

/**
 * @brief Register the Runner's chandler callback for m68k → RP
 *        report-back commands (RUNNER_CMD_DONE_EXECUTE etc.).
 *        Called once at boot from emul_start().
 */
void runner_init(void);

#endif  // RUNNER_H
