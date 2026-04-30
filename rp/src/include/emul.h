/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

#include <stdbool.h>

/**
 * @brief
 *
 * Launches the ROM emulator application. Initializes terminal interfaces,
 * configures network and storage systems, and loads the ROM data from SD or
 * network sources. Manages the main loop which includes firmware bypass,
 * user interaction and potential system resets.
 */
void emul_start();

/**
 * @brief Whether the user picked Runner mode at boot ([U] in the
 *        setup menu). Set RP-side by cmdRunner because the m68k
 *        cannot write a handshake into the read-only cartridge area.
 *        Returns false for the GEMDRIVE-only [E]/[F]/countdown path.
 */
bool emul_isRunnerActive(void);

#endif  // EMUL_H
