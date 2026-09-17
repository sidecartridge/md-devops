#include "sdcard.h"

#include "health.h"
#include "diskio.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static FATFS *mountedFsPtr = NULL;
static bool sdMounted = false;

// Remount state. The card is mounted once at boot, so a card
// pulled and put back stayed dead until a reset: FatFs keeps the volume
// registered, and with card-detect disabled on this board nothing ever marks
// the drive uninitialised, so mount_volume() sees a mounted volume and never
// asks the driver to re-initialise the card.
//
// Unregistering the volume is what breaks that: with fs_type cleared, the next
// f_mount() runs disk_initialize(), which calls the card's own init
// unconditionally (fatfs-sdk src/glue.c), and a freshly inserted card comes up.
static FATFS *bootFsPtr = NULL;
static char bootFolder[SDCARD_FOLDER_NAME_MAX] = "";
static absolute_time_t nextRemountAt;
static bool remountScheduled = false;
static uint32_t remountAttempts = 0;
static uint32_t remountRecoveries = 0;

// qsort comparator: directories first, then alphabetic case-insensitive.
static int dirFirstCmp(const void *a, const void *b) {
  const DirEntry *e1 = (const DirEntry *)a;
  const DirEntry *e2 = (const DirEntry *)b;
  if (e1->is_dir && !e2->is_dir) return -1;
  if (!e1->is_dir && e2->is_dir) return 1;
  return strcasecmp(e1->name, e2->name);
}

static void sdcard_warnDebugRisk(void) {
  size_t sdCount = sd_get_num();
  for (size_t i = 0; i < sdCount; i++) {
    sd_card_t *sdCard = sd_get_by_num(i);
    if ((sdCard != NULL) && !sdCard->use_card_detect) {
      DPRINTF(
          "WARNING: SD card-detect disabled on slot %u. "
          "When debugging, starting without an SD card can trigger assertions "
          "during init.\n",
          (unsigned)i);
    }
  }
}

static sdcard_status_t sdcardInit() {
  DPRINTF("Initializing SD card...\n");
  sdcard_warnDebugRisk();
  // Initialize the SD card
  bool success = sd_init_driver();
  if (!success) {
    DPRINTF("ERROR: Could not initialize SD card\r\n");
    return SDCARD_INIT_ERROR;
  }
  DPRINTF("SD card initialized.\n");

  sdcard_setSpiSpeedSettings();
  return SDCARD_INIT_OK;
}

FRESULT sdcard_mountFilesystem(FATFS *fsys, const char *drive) {
  // Mount the drive
  FRESULT fres = f_mount(fsys, drive, 1);
  if (fres != FR_OK) {
    DPRINTF("ERROR: Could not mount the filesystem. Error code: %d\n", fres);
  } else {
    DPRINTF("Filesystem mounted.\n");
  }
  return fres;
}

bool sdcard_dirExist(const char *dir) {
  FILINFO fno;
  FRESULT res = f_stat(dir, &fno);

  // Check if the result is OK and if the attribute indicates it's a directory
  bool dirExist = (res == FR_OK && (fno.fattrib & AM_DIR));
  DPRINTF("Directory %s exists: %s\n", dir, dirExist ? "true" : "false");
  return dirExist;
}

sdcard_status_t sdcard_ensureFolder(const char *folderName) {
  if ((folderName == NULL) || (folderName[0] == '\0') ||
      (strcmp(folderName, "/") == 0)) {
    DPRINTF("Empty or root folder name. Ignoring.\n");
    return SDCARD_INIT_OK;
  }

  bool folderExists = sdcard_dirExist(folderName);
  DPRINTF("Folder exists: %s\n", folderExists ? "true" : "false");
  if (folderExists) {
    return SDCARD_INIT_OK;
  }

  FRESULT fres = f_mkdir(folderName);
  if (fres != FR_OK) {
    DPRINTF("Error creating the folder.\n");
    return SDCARD_CREATE_FOLDER_ERROR;
  }
  DPRINTF("Folder created.\n");
  return SDCARD_INIT_OK;
}

sdcard_status_t sdcard_initFilesystem(FATFS *fsPtr, const char *folderName) {
  sdMounted = false;
  mountedFsPtr = NULL;

  if ((fsPtr == NULL) || (folderName == NULL) || (folderName[0] == '\0')) {
    DPRINTF("Invalid SD filesystem initialization arguments.\n");
    return SDCARD_INIT_ERROR;
  }

  // Remember what to mount with before trying, not after succeeding: a device
  // booted with no card has to keep retrying too, and that is the case where
  // the retry matters most.
  bootFsPtr = fsPtr;
  snprintf(bootFolder, sizeof(bootFolder), "%s", folderName);

  // Check the status of the sd card
  sdcard_status_t sdcardOk = sdcardInit();
  if (sdcardOk != SDCARD_INIT_OK) {
    DPRINTF("Error initializing the SD card.\n");
    return SDCARD_INIT_ERROR;
  }

  // Now try to mount the filesystem
  FRESULT fres;
  fres = sdcard_mountFilesystem(fsPtr, "0:");
  if (fres != FR_OK) {
    DPRINTF("Error mounting the filesystem.\n");
    return SDCARD_MOUNT_ERROR;
  }
  DPRINTF("Filesystem mounted.\n");

  sdcard_status_t folderStatus = sdcard_ensureFolder(folderName);
  if (folderStatus != SDCARD_INIT_OK) {
    return folderStatus;
  }

  mountedFsPtr = fsPtr;
  sdMounted = true;
  return SDCARD_INIT_OK;
}

void sdcard_changeSpiSpeed(int baudRateKbits) {
  size_t sdNum = sd_get_num();
  if (sdNum > 0) {
    int baudRate = baudRateKbits;
    if (baudRate > 0) {
      DPRINTF("Changing SD card baud rate to %i\n", baudRate);
      sd_card_t *sdCard = sd_get_by_num(sdNum - 1);
      if ((sdCard == NULL) || (sdCard->spi_if_p == NULL) ||
          (sdCard->spi_if_p->spi == NULL)) {
        DPRINTF("SD card SPI interface is not available\n");
        return;
      }
      sdCard->spi_if_p->spi->baud_rate = baudRate * SDCARD_KILOBAUD;
    } else {
      DPRINTF("Invalid baud rate. Using default value\n");
    }
  } else {
    DPRINTF("SD card not found\n");
  }
}

void sdcard_setSpiSpeedSettings() {
  // Get the SPI speed from the configuration
  SettingsConfigEntry *spiSpeed =
      settings_find_entry(gconfig_getContext(), PARAM_SD_BAUD_RATE_KB);
  int baudRate = 0;
  if (spiSpeed != NULL) {
    baudRate = atoi(spiSpeed->value);
  }

  // Clamp to a sane range; PARAM_SD_BAUD_RATE_KB is just a string in
  // shared config and a stale/typoed value (e.g. 999999) would otherwise
  // ask the SPI driver to clock past what the hardware sustains.
  if (baudRate > SDCARD_MAX_KHZ) {
    DPRINTF("Baud rate too high. Clamping to %d KHz\n", SDCARD_MAX_KHZ);
    baudRate = SDCARD_MAX_KHZ;
  }
  if (baudRate < SDCARD_MIN_KHZ) {
    DPRINTF("Baud rate too low. Clamping to %d KHz\n", SDCARD_MIN_KHZ);
    baudRate = SDCARD_MIN_KHZ;
  }

  sdcard_changeSpiSpeed(baudRate);
}

void sdcard_getInfo(FATFS *fsPtr, uint32_t *totalSizeMb,
                    uint32_t *freeSpaceMb) {
  if ((fsPtr == NULL) || (totalSizeMb == NULL) || (freeSpaceMb == NULL)) {
    DPRINTF("Invalid SD card info arguments.\n");
    return;
  }

  DWORD freClust;

  // Set initial values to zero as a precaution
  *totalSizeMb = 0;
  *freeSpaceMb = 0;

  // Get volume information and free clusters of drive
  FRESULT res = f_getfree("", &freClust, &fsPtr);
  if (res != FR_OK) {
    DPRINTF("Error getting free space information: %d\n", res);
    return;  // Error handling: Set values to zero if getfree fails
  }

  // Calculate total sectors in the SD card
  uint64_t totalSectors = (fsPtr->n_fatent - 2) * fsPtr->csize;

  // Convert total sectors to bytes and then to megabytes
  *totalSizeMb = (totalSectors * NUM_BYTES_PER_SECTOR) / SDCARD_MEGABYTE;

  // Convert free clusters to sectors and then to bytes
  uint64_t freeSpaceBytes =
      (uint64_t)freClust * fsPtr->csize * NUM_BYTES_PER_SECTOR;

  // Convert bytes to megabytes
  *freeSpaceMb = freeSpaceBytes / SDCARD_MEGABYTE;
}

bool sdcard_isMounted(void) { return sdMounted && (mountedFsPtr != NULL); }

uint32_t sdcard_getRemountRecoveries(void) { return remountRecoveries; }

// Whether the card is still there cannot be inferred from FatFs results: with
// the card physically out, `volume` still answered 200 from the cached FAT and
// a listing answered 200 with an empty directory, because FatFs serves what it
// has cached and never reports a disk error. Card-detect is disabled on this
// board, so disk_status() cannot tell us either.
//
// The only honest question is a real one: read a sector off the card. Sector 0
// is always there, the read bypasses FatFs's cache, and a card that has been
// pulled cannot answer it.
static absolute_time_t nextPresenceCheckAt;
static bool presenceCheckStarted = false;
static uint8_t presenceBuf[512];

static void sdcard_pollPresence(void) {
  absolute_time_t now = get_absolute_time();
  if (!presenceCheckStarted) {
    presenceCheckStarted = true;
    nextPresenceCheckAt = delayed_by_ms(now, SDCARD_PRESENCE_POLL_MS);
    return;
  }
  if (absolute_time_diff_us(now, nextPresenceCheckAt) > 0) {
    return;
  }
  nextPresenceCheckAt = delayed_by_ms(now, SDCARD_PRESENCE_POLL_MS);

  DRESULT dres = disk_read(0, presenceBuf, 0, 1);
  if (dres == RES_OK) {
    return;
  }
  DPRINTF("SD card: sector read failed (%d) -- card gone\n", (int)dres);
  sdMounted = false;
  mountedFsPtr = NULL;
  remountScheduled = true;
  nextRemountAt = now;
}

void sdcard_noteResult(FRESULT fres) {
  if (!sdMounted) {
    return;
  }
  // Only the results that mean "the medium is not answering". A missing file or
  // a full lock table says nothing about the card.
  if (fres == FR_DISK_ERR || fres == FR_NOT_READY || fres == FR_INVALID_DRIVE ||
      fres == FR_NO_FILESYSTEM) {
    DPRINTF("SD card: marking unmounted after FatFs error %d\n", (int)fres);
    sdMounted = false;
    mountedFsPtr = NULL;
    remountScheduled = true;
    nextRemountAt = get_absolute_time();  // try at the next poll
  }
}

// Called from the main loop. Cheap when the card is mounted; when it is not,
// retries a full remount every SDCARD_REMOUNT_RETRY_MS.
void sdcard_pollRemount(void) {
  if (sdcard_isMounted()) {
    sdcard_pollPresence();
    return;
  }
  if (bootFsPtr == NULL || bootFolder[0] == '\0') {
    return;
  }
  if (!remountScheduled) {
    remountScheduled = true;
    nextRemountAt = get_absolute_time();
  }
  if (absolute_time_diff_us(get_absolute_time(), nextRemountAt) > 0) {
    return;
  }

  // A remount talks to the card over SPI and can take a moment, and a card that
  // is half-inserted can take longer still.
  health_feed();
  health_setPhase(HEALTH_PHASE_BOOT);
  remountAttempts++;
  // Two things have to be undone before a reinserted card will come up.
  //
  // First the driver: sd_card_spi_init() returns immediately unless STA_NOINIT
  // is set ("Check if we're not already initialized before proceeding",
  // sd_card_spi.c), and with card-detect disabled nothing ever sets it when a
  // card is pulled. So the driver would keep believing the old card is still
  // initialised and skip the whole init sequence, leaving FatFs to fail on the
  // first sector read. deinit() sets STA_NOINIT and forgets the card type.
  //
  // Then FatFs: while the volume stays registered, mount_volume() will not ask
  // the driver to initialise anything at all.
  sd_card_t *sdCard = sd_get_by_num(0);
  if (sdCard != NULL && sdCard->deinit != NULL) {
    sdCard->deinit(sdCard);
  }
  f_mount(NULL, "0:", 0);
  sdcard_status_t rc = sdcard_initFilesystem(bootFsPtr, bootFolder);
  health_feed();
  if (rc == SDCARD_INIT_OK) {
    remountRecoveries++;
    presenceCheckStarted = false;
    DPRINTF("SD card: remounted after %lu attempt(s)\n",
            (unsigned long)remountAttempts);
    remountAttempts = 0;
    remountScheduled = false;
    return;
  }
  nextRemountAt = delayed_by_ms(get_absolute_time(), SDCARD_REMOUNT_RETRY_MS);
}

bool sdcard_getMountedInfo(uint32_t *totalSizeMb, uint32_t *freeSpaceMb) {
  if ((totalSizeMb == NULL) || (freeSpaceMb == NULL)) {
    return false;
  }

  *totalSizeMb = 0;
  *freeSpaceMb = 0;

  if (!sdcard_isMounted()) {
    return false;
  }

  FATFS *fs = mountedFsPtr;
  DWORD freeClusters = 0;
  FRESULT res = f_getfree("", &freeClusters, &fs);
  if ((res != FR_OK) || (fs == NULL)) {
    DPRINTF("Error getting mounted free space information: %d\n", res);
    return false;
  }

  uint64_t totalSectors = (uint64_t)(fs->n_fatent - 2U) * fs->csize;
  *totalSizeMb =
      (uint32_t)((totalSectors * NUM_BYTES_PER_SECTOR) / SDCARD_MEGABYTE);

  uint64_t freeSpaceBytes =
      (uint64_t)freeClusters * fs->csize * NUM_BYTES_PER_SECTOR;
  *freeSpaceMb = (uint32_t)(freeSpaceBytes / SDCARD_MEGABYTE);
  return true;
}

// Ported verbatim from md-drives-emulator/sdcard.c.
FRESULT __not_in_flash_func(sdcard_loadDirectory)(
    const char *path, char entries_arr[][MAX_FILENAME_LENGTH + 1],
    uint16_t *entry_count, uint16_t *selected, uint16_t *page, bool dirs_only,
    EntryFilterFn filter_fn, char top_dir[MAX_FILENAME_LENGTH + 1]) {
  FILINFO fno;
  DIR dir;
  FRESULT res;
  *entry_count = 0;

  DirEntry temp_entries[MAX_ENTRIES_DIR];
  uint16_t temp_count = 0;

  // ".." unless we're at root or the configured top dir.
  if ((strcmp(path, "/") != 0) &&
      (top_dir != NULL && strlen(top_dir) > 0 && strcmp(path, top_dir) != 0)) {
    snprintf(entries_arr[*entry_count], MAX_FILENAME_LENGTH + 1, "..");
    (*entry_count)++;
  }

  res = f_opendir(&dir, path);
  if (res != FR_OK) {
    DPRINTF("Error opening directory: %d\n", res);
    return res;
  }

  while ((res = f_readdir(&dir, &fno)) == FR_OK && fno.fname[0]) {
    if (dirs_only && !(fno.fattrib & AM_DIR)) continue;
    if (filter_fn && !filter_fn(fno.fname, fno.fattrib)) continue;
    if (temp_count >= MAX_ENTRIES_DIR) break;

    snprintf(temp_entries[temp_count].name, MAX_FILENAME_LENGTH + 1, "%s%s",
             fno.fname, (fno.fattrib & AM_DIR) ? "/" : "");
    temp_entries[temp_count].is_dir = (fno.fattrib & AM_DIR) != 0;
    temp_count++;
  }

  f_closedir(&dir);

  qsort(temp_entries, temp_count, sizeof(DirEntry), dirFirstCmp);

  for (uint16_t i = 0; i < temp_count; i++) {
    strncpy(entries_arr[*entry_count], temp_entries[i].name,
            MAX_FILENAME_LENGTH);
    entries_arr[*entry_count][MAX_FILENAME_LENGTH] = '\0';
    (*entry_count)++;
  }

  *page = 0;
  *selected = 0;

  DPRINTF("Loaded %d entries\n", *entry_count);
  return FR_OK;
}
