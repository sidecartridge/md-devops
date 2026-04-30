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
#define RUNNER_CMD_RESET (APP_RUNNER + 0x01)  // cold reset

// RP-side state machine mirror. cmdRunner sets ACTIVE; per-command
// handlers update last_command. Used by GET /api/v1/runner.
typedef enum {
  RUNNER_LAST_NONE = 0,
  RUNNER_LAST_RESET = 1,
  // S3/S4: RUNNER_LAST_EXECUTE, RUNNER_LAST_CD.
} runner_last_command_t;

#endif  // RUNNER_H
