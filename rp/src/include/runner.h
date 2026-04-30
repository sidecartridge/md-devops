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
// Runner mode is active. ASCII 'RNV1' big-endian.
#define RUNNER_HELLO_MAGIC 0x524E5631u

// Protocol version: high u16 = min, low u16 = max. v1.
#define RUNNER_PROTO_VERSION 0x00010001u

#endif  // RUNNER_H
