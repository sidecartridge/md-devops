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
#define RUNNER_CMD_RESET (APP_RUNNER + 0x01)    // cold reset
#define RUNNER_CMD_EXECUTE (APP_RUNNER + 0x02)  // Pexec mode 0
#define RUNNER_CMD_CD (APP_RUNNER + 0x03)       // Dsetpath
#define RUNNER_CMD_RES (APP_RUNNER + 0x04)      // XBIOS Setscreen

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

// RP-side state machine mirror. cmdRunner sets ACTIVE; per-command
// handlers update last_command. Used by GET /api/v1/runner.
typedef enum {
  RUNNER_LAST_NONE = 0,
  RUNNER_LAST_RESET = 1,
  RUNNER_LAST_EXECUTE = 2,
  RUNNER_LAST_CD = 3,
  RUNNER_LAST_RES = 4,
} runner_last_command_t;

/**
 * @brief Register the Runner's chandler callback for m68k → RP
 *        report-back commands (RUNNER_CMD_DONE_EXECUTE etc.).
 *        Called once at boot from emul_start().
 */
void runner_init(void);

#endif  // RUNNER_H
