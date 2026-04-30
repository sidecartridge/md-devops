/**
 * File: sdcard.h
 * Author: Diego Parrilla Santamaría
 * Date: December 2024, February 2026
 * Copyright: 2024-2026 - GOODDATA LABS SL
 * Description: Header for sdcard.c which manages the SD card
 */

#ifndef SDCARD_H
#define SDCARD_H

#include "constants.h"
#include "debug.h"
#include "gconfig.h"
#include "sd_card.h"

typedef enum {
  SDCARD_INIT_OK = 0,
  SDCARD_INIT_ERROR = -1,
  SDCARD_MOUNT_ERROR = -2,
  SDCARD_CREATE_FOLDER_ERROR = -3
} sdcard_status_t;

#define SDCARD_KILOBAUD 1000

// Bounds enforced by sdcard_setSpiSpeedSettings() so a malformed
// PARAM_SD_BAUD_RATE_KB cannot drive SPI faster than the hardware can
// sustain or slower than is practically useful.
#define SDCARD_MAX_KHZ 24000  // 24 MHz upper bound
#define SDCARD_MIN_KHZ 1000   // 1 MHz  lower bound

#define NUM_BYTES_PER_SECTOR 512
#define SDCARD_MEGABYTE 1048576

// File/dir entry list browsable — must match md-drives-emulator/sdcard.h
// so the directory pager that the app ports can be reused as-is.
#include "settings.h"
#define MAX_ENTRIES_DIR 256
#define MAX_FILENAME_LENGTH (SETTINGS_MAX_VALUE_LENGTH - 1)

typedef struct {
  char name[MAX_FILENAME_LENGTH + 1];
  bool is_dir;
} DirEntry;

// Optional filter callback for directory listing. Return true to include
// the entry, false to skip it.
typedef bool (*EntryFilterFn)(const char *name, BYTE attr);

/**
 * @brief Mount filesystem using FatFS library.
 *
 * Attempts to mount the provided filesystem on the given drive. Incorporates
 * error checking based on FatFS return codes.
 *
 * @param fsys Pointer to a FATFS structure to be associated with the drive.
 * @param drive Drive identifier string.
 * @return FRESULT Returns the FatFS function result code.
 */
FRESULT sdcard_mountFilesystem(FATFS *fsys, const char *drive);

/**
 * @brief Verify the existence of a directory.
 *
 * Leverages FatFS f_stat to determine if the specified directory exists and is
 * accessible.
 *
 * @param dir Null-terminated string containing the directory path.
 * @return bool Returns true if the directory exists; false otherwise.
 */
bool sdcard_dirExist(const char *dir);

/**
 * @brief Initialize filesystem on SD card.
 *
 * Sets up the SD card filesystem by mounting it and preparing the designated
 * folder. Returns specific status codes reflecting success or the nature of any
 * initialization failure. The folder is created if it does not already exist.
 *
 * @param fsPtr Pointer to a FATFS structure used for filesystem mounting.
 * @param folderName Name of the folder to be created or used on the SD card.
 * @return sdcard_status_t Status code indicating the initialization result.
 */
sdcard_status_t sdcard_initFilesystem(FATFS *fsPtr, const char *folderName);

/**
 * @brief Ensure a folder exists on the mounted filesystem.
 *
 * Creates the folder when it is missing. Empty folder names and the root
 * (`"/"`) are treated as no-ops and return SDCARD_INIT_OK without
 * touching the filesystem. Useful for apps that want their own
 * subdirectory inside the SD card without re-running the full
 * sdcard_initFilesystem flow.
 *
 * @param folderName Path of the folder to be created or verified.
 * @return sdcard_status_t SDCARD_INIT_OK on success (or no-op);
 *         SDCARD_CREATE_FOLDER_ERROR if f_mkdir fails.
 */
sdcard_status_t sdcard_ensureFolder(const char *folderName);

/**
 * @brief Adjust the SPI communication speed.
 *
 * Alters the SPI baud rate using a configuration entry. Verifies the provided
 * rate and defaults to a preset value if the given baud rate is invalid.
 *
 * @param baudRateKbits Desired SPI speed in kilobits per second.
 */
void sdcard_changeSpiSpeed(int baudRateKbits);

/**
 * @brief Establish SPI speed configuration.
 *
 * Applies the SPI speed settings to align communication parameters for SD card
 * operations.
 */
void sdcard_setSpiSpeedSettings();

/**
 * @brief Retrieve SD card storage information.
 *
 * Obtains details regarding the SD card by determining both total capacity and
 * available free space in megabytes.
 *
 * @param fsPtr Pointer to the FATFS object associated with the SD card.
 * @param totalSizeMb Pointer to a variable where the total storage (in MB) is
 * stored.
 * @param freeSpaceMb Pointer to a variable where the free space (in MB) is
 * stored.
 */
void sdcard_getInfo(FATFS *fsPtr, uint32_t *totalSizeMb, uint32_t *freeSpaceMb);

/**
 * @brief Indicates whether the SD filesystem is currently mounted.
 *
 * @return true if mounted and usable; false otherwise.
 */
bool sdcard_isMounted(void);

/**
 * @brief Retrieves total and free SD card space from the mounted filesystem.
 *
 * @param totalSizeMb Output total size in MB.
 * @param freeSpaceMb Output free space in MB.
 * @return true on success, false on failure or if not mounted.
 */
bool sdcard_getMountedInfo(uint32_t *totalSizeMb, uint32_t *freeSpaceMb);

/**
 * @brief Load entries from an SD card directory.
 *
 * Mirrors md-drives-emulator/sdcard_loadDirectory. Reads the directory at
 * @p path and fills @p entries_arr with up to @c MAX_ENTRIES_DIR names,
 * sorted directories-first then alphabetically. ".." is prepended unless
 * the path is the root or @p top_dir.
 */
FRESULT __not_in_flash_func(sdcard_loadDirectory)(
    const char *path, char entries_arr[][MAX_FILENAME_LENGTH + 1],
    uint16_t *entry_count, uint16_t *selected, uint16_t *page, bool dirs_only,
    EntryFilterFn filter_fn, char top_dir[MAX_FILENAME_LENGTH + 1]);

// Hardware Configuration of SPI "objects"

// NOLINTBEGIN(readability-identifier-naming)
size_t sd_get_num();
sd_card_t *sd_get_by_num(size_t num);
size_t spi_get_num();
spi_t *spi_get_by_num(size_t num);
// NOLINTEND(readability-identifier-naming)

#endif  // SDCARD_H
