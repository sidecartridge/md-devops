/**
 * File: commemul.h
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: ROM3 communication emulator backed by a DMA ring buffer.
 */

#ifndef COMMEMUL_H
#define COMMEMUL_H

#include <inttypes.h>
#include <stdbool.h>

#include "pico/stdlib.h"

typedef void (*CommEmulSampleCallback)(uint16_t sample);

// Returns 0 on success, < 0 on failure (PIO program load failed). The
// PIO state-machine and DMA-channel claims call the SDK's "panic on
// exhaustion" variants, so those paths abort the whole boot rather
// than returning here.
int commemul_init(void);
void __not_in_flash_func(commemul_poll)(CommEmulSampleCallback callback);

// Times the DMA ring lapped the reader since boot. Each one lost up to
// a ring's worth of ROM3 samples (commands the ST retries, debug bytes).
uint32_t commemul_getOverruns(void);
/** EPIC-18 STORY-01: most samples ever found unread in one poll. */
uint32_t commemul_getMaxUnread(void);
/** EPIC-18 STORY-01: longest gap between two polls, microseconds. */
uint32_t commemul_getMaxPollGapUs(void);
/** EPIC-18 STORY-01: the ring's capacity in samples, for comparison. */
uint32_t commemul_getRingWords(void);

#endif  // COMMEMUL_H
