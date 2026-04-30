/**
 * File: gemdrive.c
 * Description: GEMDRIVE — Epic 01.
 *   S1: HELLO handshake.
 *   S2: SAVE_VECTORS / RESET_GEM.
 *   S3 Phase 1: drive bitmap publish, REENTRY_LOCK/UNLOCK, DFREE, DGETPATH.
 *   S3 Phase 2: FOPEN, FCLOSE, FSEEK, READ_BUFF, FSFIRST, FSNEXT,
 *     DTA_RELEASE — file-handle and DTA tables, FAT 8.3 pattern matcher,
 *     path translation between Atari ST conventions and the SD-card
 *     subdirectory configured by GEMDRIVE_FOLDER.
 */

#include "include/gemdrive.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "chandler.h"
#include "debug.h"
#include "emul.h"
#include "ff.h"
#include "memfunc.h"
#include "settings.h"
#include "tprotocol.h"

extern unsigned int __rom_in_ram_start__;

// Layout invariants (mirror gemdrive.h / gemdrive.s):
//   handles ≥ GEMDRIVE_FIRST_FD belong to GEMDRIVE; below = pass-through.
//   GEMDRIVE_MAX_OPEN_FILES caps how many we can hold simultaneously.

static struct {
  bool installed;
  uint32_t old_vector;
  uint32_t old_vector_cell;
  uint8_t drive_number;
  char drive_letter;
} gemdriveVectors;

static char dpathStr[GEMDRIVE_DEFAULT_PATH_LEN] = "\\";

typedef struct {
  bool inUse;
  FIL fp;
} GemFileSlot;
static GemFileSlot fileTable[GEMDRIVE_MAX_OPEN_FILES];

typedef struct {
  bool inUse;
  bool hasDir;            // true if `dir` was opened by Fsfirst
  uint32_t dtaAddr;       // m68k-side DTA pointer (key)
  uint32_t lruStamp;      // bumped each time the slot is touched
  DIR dir;                // FatFs iterator (only valid if hasDir)
  char pattern[16];       // pattern with wildcards (UPPER-CASE)
  uint8_t attribs;
  char sdDir[256];        // resolved SD path of the search directory
} GemDtaSlot;
static GemDtaSlot dtaTable[GEMDRIVE_MAX_DTAS];
static uint32_t dtaLruCounter;

// ---------------------------------------------------------------------------
// aconfig helpers
// ---------------------------------------------------------------------------

static uint32_t aconfigInt(const char *key) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), key);
  if (entry == NULL) return 0;
  return (uint32_t)strtoul(entry->value, NULL, 0);
}

static bool aconfigBool(const char *key, bool defaultValue) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), key);
  if (entry == NULL) return defaultValue;
  return strcmp(entry->value, "true") == 0;
}

static const char *aconfigString(const char *key, const char *defaultValue) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), key);
  if (entry == NULL) return defaultValue;
  return entry->value;
}

static char resolveDriveLetter(void) {
  const char *value = aconfigString(ACONFIG_PARAM_GEMDRIVE_DRIVE, "C");
  if (value == NULL || value[0] == '\0') return 'C';
  char letter = (char)toupper((unsigned char)value[0]);
  if (letter < 'A' || letter > 'P') return 'C';
  return letter;
}

// ---------------------------------------------------------------------------
// Shared-region writers. The cartridge bus presents each RP-side uint16_t
// as one word in m68k order, so SET_SHARED_VAR's "high word at +0, low
// word at +2" pattern is what the m68k expects when it reads a longword
// at the same offset. WRITE_AND_SWAP_LONGWORD has the same net effect
// (a single uint32_t store with halves reversed), used here for the
// DFREE struct fields. Single-word fields use WRITE_WORD; raw bytes
// (filename) use memcpy + CHANGE_ENDIANESS_BLOCK16.
// ---------------------------------------------------------------------------

static uint32_t sharedBaseAddress(void) {
  return (uint32_t)&__rom_in_ram_start__;
}

static uint32_t appFreeAddress(void) {
  return sharedBaseAddress() + CHANDLER_APP_FREE_OFFSET;
}

static void writeAppFreeLong(uint32_t offsetFromAppFree, uint32_t value) {
  WRITE_AND_SWAP_LONGWORD(appFreeAddress(), offsetFromAppFree, value);
}

static void writeAppFreeWord(uint32_t offsetFromAppFree, uint16_t value) {
  WRITE_WORD(appFreeAddress(), offsetFromAppFree, value);
}

static void writeAppFreeBytes(uint32_t offsetFromAppFree, const void *data,
                              size_t length) {
  uint8_t *dst = (uint8_t *)(appFreeAddress() + offsetFromAppFree);
  memcpy(dst, data, length);
}

static void writeAppFreeBytesSwapped(uint32_t offsetFromAppFree,
                                     const void *data, size_t length) {
  uint8_t *dst = (uint8_t *)(appFreeAddress() + offsetFromAppFree);
  memcpy(dst, data, length);
  // m68k reads byte-pairs in BE order; CHANGE_ENDIANESS_BLOCK16 swaps
  // each uint16 so the bytes appear in the right order to the m68k.
  CHANGE_ENDIANESS_BLOCK16(dst, length & ~1);
}

// ---------------------------------------------------------------------------
// Path translation: Atari "C:\FOO\BAR.PRG" → SD "/devops/FOO/BAR.PRG"
// ---------------------------------------------------------------------------

// Direct port of md-drives-emulator's getLocalFullPathname. Translates
// an Atari path (drive-letter-prefixed, '\'-separated, possibly
// relative to dpathStr) into an SD path beneath GEMDRIVE_FOLDER.
//
// Three cases:
//   1. "X:..." → strip drive letter, take what follows verbatim under
//      hdFolder (ignore dpathStr).
//   2. "\foo"  → absolute on the emulated drive, take verbatim under
//      hdFolder (ignore dpathStr).
//   3. "foo"   → relative, prepend hdFolder + dpathStr.
static void getLocalFullPathname(const char *atariPath, char *out,
                                 size_t outSize) {
  const char *folder =
      aconfigString(ACONFIG_PARAM_GEMDRIVE_FOLDER, "/devops");

  char tmpPath[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  char working[GEMDRIVE_DEFAULT_PATH_LEN] = {0};

  // Make a working copy because case 1 trims the drive prefix.
  strncpy(working, atariPath, sizeof(working) - 1);

  if (working[1] == ':') {
    char shifted[GEMDRIVE_DEFAULT_PATH_LEN];
    snprintf(shifted, sizeof(shifted), "%s", working + 2);
    strncpy(working, shifted, sizeof(working) - 1);
    snprintf(tmpPath, sizeof(tmpPath), "%s/", folder);
  } else if (working[0] == '\\') {
    snprintf(tmpPath, sizeof(tmpPath), "%s/", folder);
  } else {
    if (dpathStr[1] == ':') {
      snprintf(tmpPath, sizeof(tmpPath), "%s/%s", folder, dpathStr + 2);
    } else {
      snprintf(tmpPath, sizeof(tmpPath), "%s/%s", folder, dpathStr);
    }
  }

  snprintf(out, outSize, "%s/%s", tmpPath, working);

  // Convert backslashes to forward slashes (sdcard_back2ForwardSlash).
  for (char *p = out; *p; p++) {
    if (*p == '\\') *p = '/';
  }

  // Collapse duplicate slashes (sdcard_removeDupSlashes).
  char *read = out;
  char *write = out;
  bool prevSlash = false;
  while (*read) {
    if (*read == '/') {
      if (!prevSlash) {
        *write++ = '/';
        prevSlash = true;
      }
    } else {
      *write++ = *read;
      prevSlash = false;
    }
    read++;
  }
  *write = '\0';
}

// ---------------------------------------------------------------------------
// FAT 8.3 wildcard matcher. Pattern can use '*' and '?'. Compares
// case-insensitively; FatFs returns LFN-friendly names but on FAT
// volumes the short name is what the Atari sees.
// ---------------------------------------------------------------------------

static bool wildcardMatch(const char *pat, const char *name) {
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (*pat == '\0') return true;
      while (*name) {
        if (wildcardMatch(pat, name)) return true;
        name++;
      }
      return false;
    }
    if (*name == '\0') return false;
    if (*pat != '?' &&
        toupper((unsigned char)*pat) != toupper((unsigned char)*name)) {
      return false;
    }
    pat++;
    name++;
  }
  return *name == '\0';
}

// ---------------------------------------------------------------------------
// File-handle table
// ---------------------------------------------------------------------------

static int allocFileSlot(void) {
  for (int i = 0; i < GEMDRIVE_MAX_OPEN_FILES; i++) {
    if (!fileTable[i].inUse) {
      fileTable[i].inUse = true;
      memset(&fileTable[i].fp, 0, sizeof(FIL));
      return i;
    }
  }
  return -1;
}

static GemFileSlot *fileSlotByHandle(uint16_t handle) {
  if (handle < GEMDRIVE_FIRST_FD) return NULL;
  int idx = handle - GEMDRIVE_FIRST_FD;
  if (idx < 0 || idx >= GEMDRIVE_MAX_OPEN_FILES) return NULL;
  if (!fileTable[idx].inUse) return NULL;
  return &fileTable[idx];
}

static int handleFromSlotIndex(int idx) { return GEMDRIVE_FIRST_FD + idx; }

static void releaseFileSlot(int idx) {
  if (idx < 0 || idx >= GEMDRIVE_MAX_OPEN_FILES) return;
  fileTable[idx].inUse = false;
}

// ---------------------------------------------------------------------------
// DTA tracking
// ---------------------------------------------------------------------------

static GemDtaSlot *findDtaSlot(uint32_t dtaAddr) {
  for (int i = 0; i < GEMDRIVE_MAX_DTAS; i++) {
    if (dtaTable[i].inUse && dtaTable[i].dtaAddr == dtaAddr) {
      dtaTable[i].lruStamp = ++dtaLruCounter;
      return &dtaTable[i];
    }
  }
  return NULL;
}

// LRU-evicting allocator. Source uses an uncapped malloc'd linked list;
// we approximate with a fixed table + LRU eviction so the table can
// never refuse an Fsfirst (which would crash the desktop's directory
// walk). The slot least-recently-touched wins eviction.
static GemDtaSlot *allocDtaSlot(uint32_t dtaAddr) {
  GemDtaSlot *existing = findDtaSlot(dtaAddr);
  if (existing != NULL) {
    if (existing->hasDir) {
      f_closedir(&existing->dir);
    }
    memset(existing, 0, sizeof(*existing));
    existing->inUse = true;
    existing->dtaAddr = dtaAddr;
    existing->lruStamp = ++dtaLruCounter;
    return existing;
  }
  for (int i = 0; i < GEMDRIVE_MAX_DTAS; i++) {
    if (!dtaTable[i].inUse) {
      memset(&dtaTable[i], 0, sizeof(dtaTable[i]));
      dtaTable[i].inUse = true;
      dtaTable[i].dtaAddr = dtaAddr;
      dtaTable[i].lruStamp = ++dtaLruCounter;
      return &dtaTable[i];
    }
  }
  // Table full — evict LRU.
  int victim = 0;
  for (int i = 1; i < GEMDRIVE_MAX_DTAS; i++) {
    if (dtaTable[i].lruStamp < dtaTable[victim].lruStamp) victim = i;
  }
  DPRINTF("GEMDRIVE DTA: evicting LRU slot %d (addr=0x%08lX)\n", victim,
          (unsigned long)dtaTable[victim].dtaAddr);
  if (dtaTable[victim].hasDir) {
    f_closedir(&dtaTable[victim].dir);
  }
  memset(&dtaTable[victim], 0, sizeof(dtaTable[victim]));
  dtaTable[victim].inUse = true;
  dtaTable[victim].dtaAddr = dtaAddr;
  dtaTable[victim].lruStamp = ++dtaLruCounter;
  return &dtaTable[victim];
}

static void releaseDtaSlot(uint32_t dtaAddr) {
  GemDtaSlot *slot = findDtaSlot(dtaAddr);
  if (slot == NULL) return;
  if (slot->hasDir) {
    f_closedir(&slot->dir);
  }
  slot->inUse = false;
  slot->hasDir = false;
}

// ---------------------------------------------------------------------------
// DTA writer — fills the GEMDRIVE_DTA_TRANSFER region from a FILINFO.
// Layout: 0..11 reserved (we put drive number at offset 12), 21=attrib,
// 22-23=DOS time, 24-25=DOS date, 26-29=size, 30-43=8.3 filename.
// ---------------------------------------------------------------------------

static void normalizeShort83(const char *src, char out[14]);

static void writeDtaFromFilinfo(const FILINFO *info) {
  // Drive number at offset 12 so m68k Fsnext can verify it.
  WRITE_AND_SWAP_LONGWORD(appFreeAddress(),
                          GEMDRIVE_DTA_TRANSFER_OFFSET + 12,
                          (uint32_t)gemdriveVectors.drive_number);
  *(volatile uint8_t *)(appFreeAddress() + GEMDRIVE_DTA_TRANSFER_OFFSET + 20) =
      (uint8_t)info->fattrib;
  WRITE_WORD(appFreeAddress(), GEMDRIVE_DTA_TRANSFER_OFFSET + 22, info->ftime);
  WRITE_WORD(appFreeAddress(), GEMDRIVE_DTA_TRANSFER_OFFSET + 24, info->fdate);
  // Match md-drives-emulator's populateDTA exactly: LSW at +28, MSW at
  // +26. (My earlier ordering had them swapped, so file sizes were
  // showing as size << 16 + size >> 16 to TOS.)
  WRITE_WORD(appFreeAddress(), GEMDRIVE_DTA_TRANSFER_OFFSET + 28,
             (uint16_t)(info->fsize & 0xFFFF));
  WRITE_WORD(appFreeAddress(), GEMDRIVE_DTA_TRANSFER_OFFSET + 26,
             (uint16_t)((info->fsize >> 16) & 0xFFFF));
  // Filename: shorten to TOS 8.3 (matches source's sdcard_filterFname /
  // upperFname / shortenFname pipeline). LFN-only names (e.g. macOS-
  // created entries) get truncated to STEM~1.EXT. Then byte-swap the
  // pairs so m68k sees them in order.
  char name[14] = {0};
  const char *src = (info->altname[0] != '\0') ? info->altname : info->fname;
  normalizeShort83(src, name);
  writeAppFreeBytesSwapped(GEMDRIVE_DTA_TRANSFER_OFFSET + 30, name, 14);
  writeAppFreeWord(GEMDRIVE_DTA_F_FOUND_OFFSET, 0);  // 0 = found
}

static void clearDtaFound(uint16_t errorCode) {
  writeAppFreeWord(GEMDRIVE_DTA_F_FOUND_OFFSET, errorCode);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void publishDriveAndReentry(uint8_t driveNumber, char driveLetter) {
  uint32_t base = sharedBaseAddress();
  SET_SHARED_VAR(GEMDRIVE_SVAR_DRIVE_NUMBER, driveNumber, base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(GEMDRIVE_SVAR_DRIVE_LETTER, (uint32_t)(uint8_t)driveLetter,
                 base, CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(GEMDRIVE_SVAR_REENTRY_TRAP, 0, base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(GEMDRIVE_SVAR_FIRST_FILE_DESCRIPTOR, GEMDRIVE_FIRST_FD,
                 base, CHANDLER_SHARED_VARIABLES_OFFSET);
}

static void handleSaveVectors(uint16_t *payload) {
  uint32_t oldVec = TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint32_t cellAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);

  gemdriveVectors.old_vector = oldVec;
  gemdriveVectors.old_vector_cell = cellAddr;
  gemdriveVectors.installed = true;
  writeAppFreeBytes(GEMDRIVE_DEFAULT_PATH_OFFSET, dpathStr,
                    strlen(dpathStr) + 1);

  DPRINTF(
      "GEMDRIVE SAVE_VECTORS: trap #1 hook installed, old vector=0x%08lX, "
      "cell=0x%08lX\n",
      (unsigned long)oldVec, (unsigned long)cellAddr);
}

static void handleResetGem(void) {
  for (int i = 0; i < GEMDRIVE_MAX_OPEN_FILES; i++) {
    if (fileTable[i].inUse) {
      f_close(&fileTable[i].fp);
      fileTable[i].inUse = false;
    }
  }
  for (int i = 0; i < GEMDRIVE_MAX_DTAS; i++) {
    if (dtaTable[i].inUse) {
      if (dtaTable[i].hasDir) f_closedir(&dtaTable[i].dir);
      dtaTable[i].inUse = false;
      dtaTable[i].hasDir = false;
    }
  }
  gemdriveVectors.installed = false;
  gemdriveVectors.old_vector = 0;
  gemdriveVectors.old_vector_cell = 0;
  strcpy(dpathStr, "\\");
  writeAppFreeBytes(GEMDRIVE_DEFAULT_PATH_OFFSET, dpathStr,
                    strlen(dpathStr) + 1);
  DPRINTF("GEMDRIVE RESET_GEM: cleared file/DTA state.\n");
}

static void handleReentryLock(void) {
  SET_SHARED_VAR(GEMDRIVE_SVAR_REENTRY_TRAP, 1, sharedBaseAddress(),
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

static void handleReentryUnlock(void) {
  SET_SHARED_VAR(GEMDRIVE_SVAR_REENTRY_TRAP, 0, sharedBaseAddress(),
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

static void handleDfreeCall(void) {
  FATFS *fs = NULL;
  DWORD freeClusters = 0;
  uint32_t status = 0, totalFree = 0, totalClusters = 0;
  uint32_t bytesPerSector = 0, sectorsPerCluster = 0;

  FRESULT res = f_getfree("", &freeClusters, &fs);
  if (res != FR_OK || fs == NULL) {
    status = (uint32_t)-1;
    DPRINTF("GEMDRIVE Dfree: f_getfree failed (%d)\n", (int)res);
  } else {
    totalFree = (uint32_t)freeClusters;
    totalClusters = (uint32_t)(fs->n_fatent - 2);
#if FF_MAX_SS == FF_MIN_SS
    bytesPerSector = FF_MIN_SS;
#else
    bytesPerSector = (uint32_t)fs->ssize;
#endif
    sectorsPerCluster = (uint32_t)fs->csize;
  }

  writeAppFreeLong(GEMDRIVE_DFREE_STATUS_OFFSET, status);
  writeAppFreeLong(GEMDRIVE_DFREE_STRUCT_OFFSET + 0, totalFree);
  writeAppFreeLong(GEMDRIVE_DFREE_STRUCT_OFFSET + 4, totalClusters);
  writeAppFreeLong(GEMDRIVE_DFREE_STRUCT_OFFSET + 8, bytesPerSector);
  writeAppFreeLong(GEMDRIVE_DFREE_STRUCT_OFFSET + 12, sectorsPerCluster);
}

// Direct port of md-drives-emulator's GEMDRVEMUL_DSETPATH_CALL handler.
// Receives a 256-byte path buffer; concatenates with existing dpathStr
// if relative, replaces it if absolute. Validates with f_stat under
// hdFolder. Writes status to GEMDRIVE_SET_DPATH_STATUS.
static void handleDsetpathCall(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d3
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d4
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d5

  char incoming[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(payload, incoming,
                                    GEMDRIVE_DEFAULT_PATH_LEN);

  // Strip drive prefix if any.
  char path[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  if (incoming[1] == ':') {
    snprintf(path, sizeof(path), "%s", incoming + 2);
  } else {
    snprintf(path, sizeof(path), "%s", incoming);
  }

  // Resolve to a candidate dpathStr (TOS-side), and to the SD path.
  char newDpath[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  if (path[0] == '\\' || path[0] == '/') {
    // Absolute → replace dpathStr.
    snprintf(newDpath, sizeof(newDpath), "%s", path);
  } else {
    // Relative → concatenate.
    size_t curLen = strlen(dpathStr);
    bool needSep = (curLen == 0) || (dpathStr[curLen - 1] != '\\' &&
                                     dpathStr[curLen - 1] != '/');
    snprintf(newDpath, sizeof(newDpath), "%s%s%s", dpathStr,
             needSep ? "\\" : "", path);
  }

  // Build the SD-side absolute path under hdFolder for f_stat.
  const char *folder =
      aconfigString(ACONFIG_PARAM_GEMDRIVE_FOLDER, "/devops");
  char sdPath[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  if (newDpath[0] == '\\' || newDpath[0] == '/') {
    snprintf(sdPath, sizeof(sdPath), "%s%s", folder, newDpath);
  } else {
    snprintf(sdPath, sizeof(sdPath), "%s/%s", folder, newDpath);
  }
  for (char *p = sdPath; *p; p++) {
    if (*p == '\\') *p = '/';
  }
  // Strip trailing slash for f_stat (FatFs is picky about that).
  size_t sdLen = strlen(sdPath);
  while (sdLen > 1 && (sdPath[sdLen - 1] == '/' || sdPath[sdLen - 1] == '\\')) {
    sdPath[--sdLen] = '\0';
  }

  FILINFO info;
  FRESULT res = f_stat(sdPath, &info);
  if (res != FR_OK || !(info.fattrib & AM_DIR)) {
    DPRINTF("GEMDRIVE Dsetpath: '%s' not a directory (fr=%d, attr=0x%X)\n",
            sdPath, (int)res, (unsigned)info.fattrib);
    writeAppFreeLong(GEMDRIVE_SET_DPATH_STATUS_OFFSET, (uint32_t)-34);  // EPTHNF
    return;
  }

  strncpy(dpathStr, newDpath, sizeof(dpathStr) - 1);
  dpathStr[sizeof(dpathStr) - 1] = '\0';
  // Convert any forward slashes in dpathStr back to TOS '\\' so Dgetpath
  // returns the path the way TOS expects.
  for (char *p = dpathStr; *p; p++) {
    if (*p == '/') *p = '\\';
  }
  DPRINTF("GEMDRIVE Dsetpath: dpathStr='%s' (sd '%s')\n", dpathStr, sdPath);
  writeAppFreeLong(GEMDRIVE_SET_DPATH_STATUS_OFFSET, 0);
}

// Forward declarations: definitions live further down.
static void readPayloadAtariPath(uint16_t *payload, char *out, size_t outSize);
static void normalizeShort83(const char *src, char out[14]);
static uint8_t attribsStToFat(uint8_t stAttribs);

// ---- S5: Fsetdta / DTA exist / Pexec / SAVE_BASEPAGE / SAVE_EXEC_HEADER ----

static void handleFsetdtaCall(uint16_t *payload) {
  uint32_t dtaAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);
  // Match md-drives-emulator's insertDTA: if a slot for this address
  // already exists, leave its iterator state alone — Fsetdta is just
  // "register this DTA address", and the desktop calls it before AND
  // after Fsfirst, so wiping the existing slot would invalidate the
  // FatFs DIR mid-iteration (Fsnext would see fr=9, FR_INVALID_OBJECT).
  if (findDtaSlot(dtaAddr) != NULL) {
    return;
  }
  for (int i = 0; i < GEMDRIVE_MAX_DTAS; i++) {
    if (!dtaTable[i].inUse) {
      memset(&dtaTable[i], 0, sizeof(dtaTable[i]));
      dtaTable[i].inUse = true;
      dtaTable[i].dtaAddr = dtaAddr;
      return;
    }
  }
  DPRINTF("GEMDRIVE Fsetdta: table full (addr=0x%08lX)\n",
          (unsigned long)dtaAddr);
}

static void handleDtaExistCall(uint16_t *payload) {
  uint32_t dtaAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);
  GemDtaSlot *slot = findDtaSlot(dtaAddr);
  uint32_t result = (slot != NULL) ? dtaAddr : 0;
  writeAppFreeLong(GEMDRIVE_DTA_EXIST_OFFSET, result);
}

// PEXEC_CALL ships the 32-byte GEMDOS args frame at a0. Source's
// handler reads d3=a0 (stack addr), then walks 6 words into the buffer
// to skip past the frame's leading 6 bytes (PC.l + SR.w + funcCode.w =
// 8 bytes, but the protocol layout starts the buffer 8 words into
// payload — see md-drives-emulator/rp/src/gemdrive.c line 2170).
static void handlePexecCall(uint16_t *payload) {
  uint32_t stackAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);

  // Source skips 6 words to reach the buffer start (4 d-regs + framing),
  // then 4 more words to land on the args frame inside the buffer. That
  // puts us at the m68k func code word — followed by mode at +0,
  // fname.l at +2, cmdline.l at +6, envstr.l at +10.
  uint16_t *args = payload + 6 + 4;
  uint16_t mode = TPROTO_GET_PAYLOAD_PARAM16(args);
  args++;
  uint32_t fname = SWAP_LONGWORD(TPROTO_GET_PAYLOAD_PARAM32(args));
  args += 2;
  uint32_t cmdline = SWAP_LONGWORD(TPROTO_GET_PAYLOAD_PARAM32(args));
  args += 2;
  uint32_t envstr = SWAP_LONGWORD(TPROTO_GET_PAYLOAD_PARAM32(args));

  DPRINTF("GEMDRIVE Pexec: mode=0x%X stack=0x%08lX fname=0x%08lX\n",
          (unsigned)mode, (unsigned long)stackAddr, (unsigned long)fname);

  WRITE_WORD(appFreeAddress(), GEMDRIVE_PEXEC_MODE_OFFSET, mode);
  WRITE_AND_SWAP_LONGWORD(appFreeAddress(), GEMDRIVE_PEXEC_STACK_ADDR_OFFSET,
                          stackAddr);
  WRITE_AND_SWAP_LONGWORD(appFreeAddress(), GEMDRIVE_PEXEC_FNAME_OFFSET, fname);
  WRITE_AND_SWAP_LONGWORD(appFreeAddress(), GEMDRIVE_PEXEC_CMDLINE_OFFSET,
                          cmdline);
  WRITE_AND_SWAP_LONGWORD(appFreeAddress(), GEMDRIVE_PEXEC_ENVSTR_OFFSET,
                          envstr);
}

// SAVE_EXEC_HEADER: m68k just shipped 28 bytes of PRG header, copy as-is.
static void handleSaveExecHeader(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d3
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d4
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d5
  uint8_t *dst =
      (uint8_t *)(appFreeAddress() + GEMDRIVE_EXEC_HEADER_OFFSET);
  memcpy(dst, payload, 28);
  DPRINTF("GEMDRIVE Pexec: exec header saved (28 bytes)\n");
}

// SAVE_BASEPAGE: 256 bytes of basepage.
static void handleSaveBasepage(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint8_t *dst = (uint8_t *)(appFreeAddress() + GEMDRIVE_EXEC_PD_OFFSET);
  memcpy(dst, payload, 256);
  DPRINTF("GEMDRIVE Pexec: basepage saved (256 bytes)\n");
}

// ---- S4 write-side handlers (direct ports of md-drives-emulator) ----

static void handleDcreateCall(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d3
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d4
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d5
  char atari[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  readPayloadAtariPath(payload, atari, sizeof(atari));
  char sdPath[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  getLocalFullPathname(atari, sdPath, sizeof(sdPath));
  FRESULT res = f_mkdir(sdPath);
  uint16_t status = (res == FR_OK)         ? 0
                    : (res == FR_NO_PATH) ? (uint16_t)-34   // EPTHNF
                                          : (uint16_t)-36;  // EACCDN
  DPRINTF("GEMDRIVE Dcreate: '%s' -> fr=%d\n", sdPath, (int)res);
  writeAppFreeWord(GEMDRIVE_DCREATE_STATUS_OFFSET, status);
}

static void handleDdeleteCall(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  char atari[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  readPayloadAtariPath(payload, atari, sizeof(atari));
  char sdPath[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  getLocalFullPathname(atari, sdPath, sizeof(sdPath));
  FRESULT res = f_unlink(sdPath);
  uint16_t status;
  if (res == FR_OK) {
    status = 0;
  } else if (res == FR_DENIED) {
    status = (uint16_t)-36;  // EACCDN — non-empty
  } else if (res == FR_NO_PATH || res == FR_NO_FILE) {
    status = (uint16_t)-34;  // EPTHNF
  } else {
    status = (uint16_t)-65;  // EINTRN
  }
  DPRINTF("GEMDRIVE Ddelete: '%s' -> fr=%d\n", sdPath, (int)res);
  writeAppFreeWord(GEMDRIVE_DDELETE_STATUS_OFFSET, status);
}

static void handleFcreateCall(uint16_t *payload) {
  uint16_t mode = TPROTO_GET_PAYLOAD_PARAM16(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  char atari[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  readPayloadAtariPath(payload, atari, sizeof(atari));
  char sdPath[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  getLocalFullPathname(atari, sdPath, sizeof(sdPath));

  int slotIdx = allocFileSlot();
  if (slotIdx < 0) {
    writeAppFreeWord(GEMDRIVE_FCREATE_HANDLE_OFFSET, (uint16_t)-35);  // ENHNDL
    return;
  }
  FRESULT res = f_open(&fileTable[slotIdx].fp, sdPath,
                       FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
  if (res != FR_OK) {
    DPRINTF("GEMDRIVE Fcreate: '%s' -> fr=%d\n", sdPath, (int)res);
    releaseFileSlot(slotIdx);
    writeAppFreeWord(GEMDRIVE_FCREATE_HANDLE_OFFSET, (uint16_t)-34);  // EPTHNF
    return;
  }
  // Match source: apply the GEMDOS attrib mode via f_chmod immediately
  // after the open. Mask is AM_RDO|AM_HID|AM_SYS — those are the bits
  // FatFs lets us toggle.
  BYTE wantAttribs = (BYTE)attribsStToFat((uint8_t)(mode & 0xFF));
  FRESULT chmodRes =
      f_chmod(sdPath, wantAttribs, AM_RDO | AM_HID | AM_SYS);
  if (chmodRes != FR_OK) {
    DPRINTF("GEMDRIVE Fcreate: f_chmod('%s', 0x%X) failed (%d)\n", sdPath,
            (unsigned)wantAttribs, (int)chmodRes);
    f_close(&fileTable[slotIdx].fp);
    releaseFileSlot(slotIdx);
    writeAppFreeWord(GEMDRIVE_FCREATE_HANDLE_OFFSET, (uint16_t)-36);  // EACCDN
    return;
  }
  int handle = handleFromSlotIndex(slotIdx);
  DPRINTF("GEMDRIVE Fcreate: '%s' (mode=0x%X attribs=0x%X) -> handle %d\n",
          sdPath, (unsigned)mode, (unsigned)wantAttribs, handle);
  writeAppFreeWord(GEMDRIVE_FCREATE_HANDLE_OFFSET, (uint16_t)handle);
}

static void handleFdeleteCall(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  char atari[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  readPayloadAtariPath(payload, atari, sizeof(atari));
  char sdPath[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  getLocalFullPathname(atari, sdPath, sizeof(sdPath));
  FRESULT res = f_unlink(sdPath);
  uint32_t status;
  if (res == FR_OK || res == FR_NO_FILE) {
    status = 0;  // source treats FR_NO_FILE as success
  } else if (res == FR_DENIED) {
    status = (uint32_t)-36;
  } else if (res == FR_NO_PATH) {
    status = (uint32_t)-34;
  } else {
    status = (uint32_t)-65;
  }
  DPRINTF("GEMDRIVE Fdelete: '%s' -> fr=%d\n", sdPath, (int)res);
  writeAppFreeLong(GEMDRIVE_FDELETE_STATUS_OFFSET, status);
}

static void handleFattribCall(uint16_t *payload) {
  uint16_t flag = TPROTO_GET_PAYLOAD_PARAM16(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint16_t newAttribs = TPROTO_GET_PAYLOAD_PARAM16(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  char atari[GEMDRIVE_DEFAULT_PATH_LEN] = {0};
  readPayloadAtariPath(payload, atari, sizeof(atari));
  char sdPath[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  getLocalFullPathname(atari, sdPath, sizeof(sdPath));

  FILINFO info;
  FRESULT res = f_stat(sdPath, &info);
  if (res != FR_OK) {
    writeAppFreeLong(GEMDRIVE_FATTRIB_STATUS_OFFSET, (uint32_t)-33);
    return;
  }
  if (flag != 0) {
    // Set new attributes (low byte). FatFs accepts AM_RDO/AM_HID/AM_SYS.
    BYTE fatAttr = (BYTE)(newAttribs & (AM_RDO | AM_HID | AM_SYS));
    res = f_chmod(sdPath, fatAttr, AM_RDO | AM_HID | AM_SYS);
    if (res != FR_OK) {
      writeAppFreeLong(GEMDRIVE_FATTRIB_STATUS_OFFSET, (uint32_t)-36);
      return;
    }
    writeAppFreeLong(GEMDRIVE_FATTRIB_STATUS_OFFSET,
                     (uint32_t)(uint8_t)fatAttr);
  } else {
    writeAppFreeLong(GEMDRIVE_FATTRIB_STATUS_OFFSET,
                     (uint32_t)(uint8_t)info.fattrib);
  }
  DPRINTF("GEMDRIVE Fattrib: '%s' flag=%u newAttribs=0x%X fr=%d\n", sdPath,
          (unsigned)flag, (unsigned)newAttribs, (int)res);
}

static void handleFrenameCall(uint16_t *payload) {
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  // Buffer is 256 bytes: [0..127] src, [128..255] dst (m68k handler packs them).
  char srcAtari[128] = {0};
  char dstAtari[128] = {0};
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(payload, srcAtari, 128);
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(payload + 64, dstAtari, 128);
  srcAtari[127] = '\0';
  dstAtari[127] = '\0';

  char srcSd[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  char dstSd[GEMDRIVE_DEFAULT_PATH_LEN + 64] = {0};
  getLocalFullPathname(srcAtari, srcSd, sizeof(srcSd));
  getLocalFullPathname(dstAtari, dstSd, sizeof(dstSd));

  FRESULT res = f_rename(srcSd, dstSd);
  uint32_t status;
  if (res == FR_OK) {
    status = 0;
  } else if (res == FR_NO_FILE || res == FR_NO_PATH) {
    status = (uint32_t)-33;
  } else if (res == FR_DENIED || res == FR_EXIST) {
    status = (uint32_t)-36;
  } else {
    status = (uint32_t)-65;
  }
  DPRINTF("GEMDRIVE Frename: '%s' -> '%s' fr=%d\n", srcSd, dstSd, (int)res);
  writeAppFreeLong(GEMDRIVE_FRENAME_STATUS_OFFSET, status);
}

// Fdatime: get or set a file's DOS date/time. Mirrors source's
// CMD_FDATETIME_CALL handler. Args (post-token): d3=flag (0=get, 1=set),
// d4=handle, d5=DOS time word in low half, d6=DOS date word in low half.
// On get: read fattrib's date/time from f_stat by looking up the open
// file's path (we keep one in the slot for tracking — see below).
static void handleFdatetimeCall(uint16_t *payload) {
  uint16_t flag = TPROTO_GET_PAYLOAD_PARAM16(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint16_t handle = TPROTO_GET_PAYLOAD_PARAM16(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint32_t dosTime = TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint32_t dosDate = TPROTO_GET_PAYLOAD_PARAM32(payload);

  GemFileSlot *slot = fileSlotByHandle(handle);
  if (slot == NULL) {
    writeAppFreeLong(GEMDRIVE_FDATETIME_STATUS_OFFSET, (uint32_t)-37);
    return;
  }

  // FatFs doesn't expose date/time directly from FIL; the practical
  // approach matches source: report the values that the m68k sent us
  // back unchanged on get (the desktop typically only reads file dates
  // via Fsfirst, not via Fdatime), and accept set silently. A more
  // faithful "get" would require recording the path on Fopen and doing
  // f_stat here; that's worth doing in S6/polish if any TOS app proves
  // to depend on read-Fdatime.
  (void)flag;
  WRITE_WORD(appFreeAddress(), GEMDRIVE_FDATETIME_TIME_OFFSET,
             (uint16_t)(dosTime & 0xFFFF));
  WRITE_WORD(appFreeAddress(), GEMDRIVE_FDATETIME_DATE_OFFSET,
             (uint16_t)(dosDate & 0xFFFF));
  writeAppFreeLong(GEMDRIVE_FDATETIME_STATUS_OFFSET, 0);
}

static void handleWriteBuffCall(uint16_t *payload) {
  uint16_t handle = TPROTO_GET_PAYLOAD_PARAM16(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint32_t bytes = TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d5

  if (bytes > GEMDRIVE_WRITE_BUFFER_SIZE) bytes = GEMDRIVE_WRITE_BUFFER_SIZE;

  GemFileSlot *slot = fileSlotByHandle(handle);
  if (slot == NULL) {
    writeAppFreeLong(GEMDRIVE_WRITE_BYTES_OFFSET, 0);
    return;
  }

  // Pull the chunk out of the wire buffer (byte-swapped per word).
  uint8_t tmp[GEMDRIVE_WRITE_BUFFER_SIZE];
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(payload, tmp, (bytes + 1) & ~1);

  UINT bw = 0;
  FRESULT res = f_write(&slot->fp, tmp, (UINT)bytes, &bw);
  if (res != FR_OK) {
    DPRINTF("GEMDRIVE Fwrite: handle=%u fr=%d\n", (unsigned)handle, (int)res);
    writeAppFreeLong(GEMDRIVE_WRITE_BYTES_OFFSET, 0);
    return;
  }
  writeAppFreeLong(GEMDRIVE_WRITE_BYTES_OFFSET, (uint32_t)bw);
}

static void handleDgetpathCall(void) {
  size_t len = strlen(dpathStr);
  if (len >= GEMDRIVE_DEFAULT_PATH_LEN) len = GEMDRIVE_DEFAULT_PATH_LEN - 1;
  writeAppFreeBytes(GEMDRIVE_DEFAULT_PATH_OFFSET, dpathStr, len);
  *(volatile uint8_t *)(appFreeAddress() + GEMDRIVE_DEFAULT_PATH_OFFSET + len) =
      0;
}

// Read a path string out of the protocol payload. m68k stored each
// pair of bytes as one BE word, but the protocol delivers each word as
// a uint16_t in RP-LE byte order — so a raw memcpy yields a string
// with every pair swapped (e.g. "DESKTOP.INF" → "EDKSOT.PNI"). Use the
// shared CHANGE_ENDIANESS helper to undo that, matching what
// md-drives-emulator's getLocalFullPathname does.
static void readPayloadAtariPath(uint16_t *payload, char *out,
                                 size_t outSize) {
  if (outSize == 0) return;
  size_t copy = outSize - 1;
  if (copy & 1) copy &= ~(size_t)1;  // round down to even for word swap
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(payload, out, copy);
  out[copy] = '\0';
  // Truncate at the NUL the m68k put in the buffer.
  for (size_t i = 0; i < copy; i++) {
    if (out[i] == '\0') return;
  }
}

static void handleFopenCall(uint16_t *payload) {
  // The send_write_sync_far macro sent: d0=cmd, d1=size, d3=mode,
  // d4 unused, d5 unused, d6=size. Then the buffer (256 bytes from a4 =
  // pattern address) follows. Chandler's ring captures both the args
  // and the bytes. payload pointer here is at the post-token args.
  //
  // First longword = mode (from d3); buffer follows from offset 16 (post
  // d3/d4/d5/d6).
  uint32_t mode = TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d4
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d5
  TPROTO_NEXT32_PAYLOAD_PTR(payload);  // skip d6

  char atariPath[256] = {0};
  readPayloadAtariPath(payload, atariPath, sizeof(atariPath));

  char sdPath[384] = {0};
  getLocalFullPathname(atariPath, sdPath, sizeof(sdPath));

  // S4: honour the GEMDOS open mode now that Fwrite is wired up.
  // 0 = read, 1 = write, 2 = read+write.
  BYTE faMode = FA_READ;
  switch (mode & 0xFFFF) {
    case 1:
      faMode = FA_WRITE;
      break;
    case 2:
      faMode = FA_READ | FA_WRITE;
      break;
    default:
      faMode = FA_READ;
      break;
  }

  int slotIdx = allocFileSlot();
  if (slotIdx < 0) {
    DPRINTF("GEMDRIVE Fopen: handle table full\n");
    writeAppFreeLong(GEMDRIVE_FOPEN_HANDLE_OFFSET, (uint32_t)-35);  // EHANDLE
    return;
  }

  FRESULT res = f_open(&fileTable[slotIdx].fp, sdPath, faMode);
  if (res != FR_OK) {
    DPRINTF("GEMDRIVE Fopen: '%s' (mode=%lu) failed (%d)\n", sdPath,
            (unsigned long)mode, (int)res);
    releaseFileSlot(slotIdx);
    writeAppFreeLong(GEMDRIVE_FOPEN_HANDLE_OFFSET, (uint32_t)-33);  // EFILNF
    return;
  }
  int handle = handleFromSlotIndex(slotIdx);
  DPRINTF("GEMDRIVE Fopen: '%s' (mode=%lu) -> handle %d\n", sdPath,
          (unsigned long)mode, handle);
  writeAppFreeLong(GEMDRIVE_FOPEN_HANDLE_OFFSET, (uint32_t)handle);
}

static void handleFcloseCall(uint16_t *payload) {
  uint16_t handle = (uint16_t)TPROTO_GET_PAYLOAD_PARAM32(payload);
  GemFileSlot *slot = fileSlotByHandle(handle);
  if (slot == NULL) {
    writeAppFreeLong(GEMDRIVE_FCLOSE_STATUS_OFFSET, (uint32_t)-37);  // EBADF
    return;
  }
  FRESULT res = f_close(&slot->fp);
  releaseFileSlot(handle - GEMDRIVE_FIRST_FD);
  writeAppFreeLong(GEMDRIVE_FCLOSE_STATUS_OFFSET,
                   (res == FR_OK) ? 0 : (uint32_t)-37);
}

static void handleFseekCall(uint16_t *payload) {
  uint32_t offset = TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint16_t handle = (uint16_t)TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint16_t mode = (uint16_t)TPROTO_GET_PAYLOAD_PARAM32(payload);

  GemFileSlot *slot = fileSlotByHandle(handle);
  if (slot == NULL) {
    writeAppFreeLong(GEMDRIVE_FSEEK_STATUS_OFFSET, (uint32_t)-37);
    return;
  }
  FSIZE_t newPos;
  int32_t signedOffset = (int32_t)offset;
  switch (mode & 0xFFFF) {
    case 0:  // SEEK_SET
      newPos = (FSIZE_t)signedOffset;
      break;
    case 1:  // SEEK_CUR
      newPos = f_tell(&slot->fp) + (FSIZE_t)signedOffset;
      break;
    case 2:  // SEEK_END
      newPos = f_size(&slot->fp) + (FSIZE_t)signedOffset;
      break;
    default:
      writeAppFreeLong(GEMDRIVE_FSEEK_STATUS_OFFSET, (uint32_t)-64);  // ERANGE
      return;
  }
  FRESULT res = f_lseek(&slot->fp, newPos);
  if (res != FR_OK) {
    writeAppFreeLong(GEMDRIVE_FSEEK_STATUS_OFFSET, (uint32_t)-64);
    return;
  }
  writeAppFreeLong(GEMDRIVE_FSEEK_STATUS_OFFSET, (uint32_t)f_tell(&slot->fp));
}

static void handleReadBuffCall(uint16_t *payload) {
  uint16_t handle = (uint16_t)TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  uint32_t bytesThisChunk = TPROTO_GET_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  /* uint32_t pendingBytes = */ TPROTO_GET_PAYLOAD_PARAM32(payload);

  GemFileSlot *slot = fileSlotByHandle(handle);
  if (slot == NULL) {
    writeAppFreeLong(GEMDRIVE_READ_BYTES_OFFSET, (uint32_t)-37);
    return;
  }

  if (bytesThisChunk > GEMDRIVE_READ_BUFFER_SIZE) {
    bytesThisChunk = GEMDRIVE_READ_BUFFER_SIZE;
  }

  uint8_t tmp[GEMDRIVE_READ_BUFFER_SIZE];
  UINT bytesRead = 0;
  FRESULT res = f_read(&slot->fp, tmp, (UINT)bytesThisChunk, &bytesRead);
  if (res != FR_OK) {
    writeAppFreeLong(GEMDRIVE_READ_BYTES_OFFSET, (uint32_t)-93);  // EIO_READ
    return;
  }
  // Copy into shared region; m68k reads byte-by-byte so swap pairs.
  writeAppFreeBytesSwapped(GEMDRIVE_READ_BUFFER_OFFSET, tmp, bytesRead);
  writeAppFreeLong(GEMDRIVE_READ_BYTES_OFFSET, (uint32_t)bytesRead);
}

static void splitDirAndPattern(const char *atariSpec, char *outDir,
                               size_t outDirSize, char *outPattern,
                               size_t outPatSize) {
  // Find the last separator. Atari uses '\\'; tolerate '/' too.
  const char *sep = NULL;
  for (const char *p = atariSpec; *p; p++) {
    if (*p == '\\' || *p == '/') sep = p;
  }
  if (sep == NULL) {
    // No separator → pattern only; directory is the cwd.
    outDir[0] = '\0';
    snprintf(outPattern, outPatSize, "%s", atariSpec);
  } else {
    size_t dirLen = (size_t)(sep - atariSpec) + 1;  // include the slash
    if (dirLen >= outDirSize) dirLen = outDirSize - 1;
    memcpy(outDir, atariSpec, dirLen);
    outDir[dirLen] = '\0';
    snprintf(outPattern, outPatSize, "%s", sep + 1);
  }
  if (outPattern[0] == '\0') {
    snprintf(outPattern, outPatSize, "*.*");
  }
  for (char *p = outPattern; *p; p++) {
    *p = (char)toupper((unsigned char)*p);
  }

  // Match md-drives-emulator/searchPath2ST: strip a trailing ".*" so
  // the FatFs glob matches folders / extension-less files. Without
  // this "*.*" rejects every entry that has no '.' in its name —
  // exactly why folders weren't showing up.
  size_t patLen = strlen(outPattern);
  if (patLen >= 2 && outPattern[patLen - 1] == '*' &&
      outPattern[patLen - 2] == '.') {
    outPattern[patLen - 2] = '\0';
  }
}

// Skip noise that desktops shouldn't see and that md-drives-emulator
// also filters: dotfiles ("._foo" macOS metadata, "." / ".." entries).
static bool isHiddenEntry(const FILINFO *info) {
  if (info->fname[0] == '.') return true;
  if (info->fname[0] == '\0') return true;
  return false;
}

// FAT→ST attribute mask — direct port of source's sdcard_attribsFAT2ST.
static uint8_t attribsFatToSt(uint8_t fatAttribs) {
  return fatAttribs & (AM_RDO | AM_HID | AM_SYS | AM_DIR | AM_ARC);
}

// ST→FAT attribute mask — direct port of source's sdcard_attribsST2FAT.
// FS_ST_* bit positions match AM_* exactly for read-only/hidden/system/
// directory/archive, so masking on those bits gives the right FatFs
// attribute byte.
static uint8_t attribsStToFat(uint8_t stAttribs) {
  return stAttribs & (AM_RDO | AM_HID | AM_SYS | AM_DIR | AM_ARC);
}

// Returns true when this entry should be visible per the caller's
// `attribs` mask. Source: `(attribs & sdcard_attribsFAT2ST(fattrib))`,
// with FS_ST_ARCH OR'd into attribs unless FS_ST_LABEL was already set.
static bool entryMatchesAttribs(const FILINFO *info, uint8_t attribs) {
  uint8_t st = attribsFatToSt(info->fattrib);
  uint8_t want = attribs;
  // Normal files always have ARCH set in FAT — match source's "OR ARCH
  // unless LABEL bit was set in the request" so apps asking for plain
  // files don't get filtered out.
  if (!(want & 0x08 /* FS_ST_LABEL */)) {
    want |= 0x20 /* FS_ST_ARCH */;
  }
  return (want & st) != 0;
}

// Skip dot-prefixed entries AND entries that don't match the caller's
// attribs mask. Mirrors the source's combined while-loop.
static FRESULT advancePastFiltered(DIR *dir, FILINFO *info, uint8_t attribs) {
  FRESULT res = FR_OK;
  while (res == FR_OK && info->fname[0] &&
         (isHiddenEntry(info) || !entryMatchesAttribs(info, attribs))) {
    res = f_findnext(dir, info);
  }
  return res;
}

// Direct port of source's sdcard_shortenFname: produces a TOS-friendly
// 8.3 short name from a (possibly long, mixed-case, dirty) FatFs name.
// Up to 8 chars before '.', up to 3 after; if base too long, suffix
// "~1" before truncating; uppercase; strip everything that isn't an
// alphanumeric or one of the DOS-friendly punctuation characters.
static int isDosNameChar(int c) {
  static const char allowed[] = "_!@#$%^&()+=-~`;'<,>.|[]{}";
  if (c >= '0' && c <= '9') return 1;
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= 'a' && c <= 'z') return 1;
  for (size_t i = 0; allowed[i]; i++) {
    if ((char)c == allowed[i]) return 1;
  }
  return 0;
}

static void normalizeShort83(const char *src, char out[14]) {
  // Filter + upper-case in one pass.
  char filtered[14] = {0};
  size_t fi = 0;
  for (size_t i = 0; src[i] && fi < 13; i++) {
    int c = (unsigned char)src[i];
    if (isDosNameChar(c)) {
      filtered[fi++] = (char)toupper(c);
    }
  }
  filtered[fi] = '\0';

  // Split on the LAST '.' (FAT 8.3 convention).
  char base[9] = {0};
  char ext[4] = {0};
  const char *dot = strrchr(filtered, '.');
  size_t baseLen = (dot && dot != filtered) ? (size_t)(dot - filtered)
                                            : strlen(filtered);
  if (baseLen > 8) {
    // Long base — truncate to 6 chars + "~1".
    memcpy(base, filtered, 6);
    base[6] = '~';
    base[7] = '1';
    base[8] = '\0';
  } else {
    memcpy(base, filtered, baseLen);
    base[baseLen] = '\0';
  }
  if (dot && dot != filtered) {
    size_t extLen = strlen(dot + 1);
    if (extLen > 3) extLen = 3;
    memcpy(ext, dot + 1, extLen);
    ext[extLen] = '\0';
  }

  if (ext[0]) {
    snprintf(out, 14, "%s.%s", base, ext);
  } else {
    snprintf(out, 14, "%s", base);
  }
}

static void handleFsfirstCall(uint16_t *payload) {
  // Match md-drives-emulator's payload-walk: 1 PARAM32 + 2 NEXT32_PARAM32
  // + 1 NEXT32 advances `payload` to the start of the pattern buffer.
  uint32_t dtaAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);
  uint32_t attribs = TPROTO_GET_NEXT32_PAYLOAD_PARAM32(payload);
  uint32_t patternAddr = TPROTO_GET_NEXT32_PAYLOAD_PARAM32(payload);
  TPROTO_NEXT32_PAYLOAD_PTR(payload);
  (void)patternAddr;

  char atariSpec[192] = {0};
  readPayloadAtariPath(payload, atariSpec, sizeof(atariSpec));
  DPRINTF("GEMDRIVE Fsfirst: dtaAddr=0x%08lX attribs=0x%lX spec='%s'\n",
          (unsigned long)dtaAddr, (unsigned long)attribs, atariSpec);

  char atariDir[192] = {0};
  char pattern[16] = {0};
  splitDirAndPattern(atariSpec, atariDir, sizeof(atariDir), pattern,
                     sizeof(pattern));

  char sdDir[256] = {0};
  getLocalFullPathname(atariDir[0] ? atariDir : "", sdDir, sizeof(sdDir));

  size_t dirLen = strlen(sdDir);
  while (dirLen > 1 && (sdDir[dirLen - 1] == '/' || sdDir[dirLen - 1] == '\\')) {
    sdDir[--dirLen] = '\0';
  }

  GemDtaSlot *slot = allocDtaSlot(dtaAddr);
  if (slot == NULL) {
    DPRINTF("GEMDRIVE Fsfirst: DTA table full\n");
    clearDtaFound(0xFFFF);
    return;
  }
  strncpy(slot->pattern, pattern, sizeof(slot->pattern) - 1);
  slot->attribs = (uint8_t)attribs;
  strncpy(slot->sdDir, sdDir, sizeof(slot->sdDir) - 1);

  // f_findfirst stores the pattern as a POINTER in slot->dir.pat (it
  // does not copy). Subsequent f_findnext calls read that pointer, so
  // the pattern memory must outlive this handler. Pass slot->pattern
  // (lives in the static dtaTable) — passing the stack-local `pattern`
  // gives us only the first hit and garbage after that.
  FILINFO info;
  FRESULT res = f_findfirst(&slot->dir, &info, sdDir, slot->pattern);
  if (res == FR_OK) {
    slot->hasDir = true;
  }
  if (res == FR_OK && info.fname[0]) {
    res = advancePastFiltered(&slot->dir, &info, slot->attribs);
  }
  if (res != FR_OK || info.fname[0] == '\0') {
    DPRINTF("GEMDRIVE Fsfirst: '%s' '%s' -> no match (fr=%d)\n", sdDir,
            pattern, (int)res);
    releaseDtaSlot(dtaAddr);
    // Match md-drives-emulator: EPTHNF (-34) when path doesn't exist,
    // EFILNF (-33) when path exists but no entries match.
    uint16_t err = (res == FR_NO_PATH) ? 0xFFDE : 0xFFDF;
    clearDtaFound(err);
    return;
  }
  DPRINTF("GEMDRIVE Fsfirst: '%s' '%s' -> %s\n", sdDir, pattern, info.fname);
  writeDtaFromFilinfo(&info);
}

static void handleFsnextCall(uint16_t *payload) {
  uint32_t dtaAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);
  GemDtaSlot *slot = findDtaSlot(dtaAddr);
  if (slot == NULL) {
    DPRINTF("GEMDRIVE Fsnext: dtaAddr=0x%08lX not in table\n",
            (unsigned long)dtaAddr);
    clearDtaFound(0xFFDF);  // GEMDOS_EFILNF (-33)
    return;
  }
  FILINFO info;
  FRESULT res = f_findnext(&slot->dir, &info);
  if (res == FR_OK && info.fname[0]) {
    res = advancePastFiltered(&slot->dir, &info, slot->attribs);
  }
  if (res != FR_OK || info.fname[0] == '\0') {
    DPRINTF("GEMDRIVE Fsnext: '%s' -> end (fr=%d)\n", slot->sdDir, (int)res);
    releaseDtaSlot(dtaAddr);
    clearDtaFound(0xFFDF);  // GEMDOS_EFILNF (-33)
    return;
  }
  DPRINTF("GEMDRIVE Fsnext: '%s' -> %s\n", slot->sdDir, info.fname);
  writeDtaFromFilinfo(&info);
}

static void handleDtaReleaseCall(uint16_t *payload) {
  uint32_t dtaAddr = TPROTO_GET_PAYLOAD_PARAM32(payload);
  releaseDtaSlot(dtaAddr);
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

void __not_in_flash_func(gemdrive_command_cb)(TransmissionProtocol *protocol,
                                              uint16_t *payload) {
  switch (protocol->command_id) {
    case GEMDRIVE_CMD_SAVE_VECTORS:
      handleSaveVectors(payload);
      return;
    case GEMDRIVE_CMD_RESET_GEM:
      handleResetGem();
      return;
    case GEMDRIVE_CMD_REENTRY_LOCK:
      handleReentryLock();
      return;
    case GEMDRIVE_CMD_REENTRY_UNLOCK:
      handleReentryUnlock();
      return;
    case GEMDRIVE_CMD_DFREE_CALL:
      handleDfreeCall();
      return;
    case GEMDRIVE_CMD_DGETPATH_CALL:
      handleDgetpathCall();
      return;
    case GEMDRIVE_CMD_DSETPATH_CALL:
      handleDsetpathCall(payload);
      return;
    case GEMDRIVE_CMD_DCREATE_CALL:
      handleDcreateCall(payload);
      return;
    case GEMDRIVE_CMD_DDELETE_CALL:
      handleDdeleteCall(payload);
      return;
    case GEMDRIVE_CMD_FCREATE_CALL:
      handleFcreateCall(payload);
      return;
    case GEMDRIVE_CMD_FDELETE_CALL:
      handleFdeleteCall(payload);
      return;
    case GEMDRIVE_CMD_FATTRIB_CALL:
      handleFattribCall(payload);
      return;
    case GEMDRIVE_CMD_FRENAME_CALL:
      handleFrenameCall(payload);
      return;
    case GEMDRIVE_CMD_WRITE_BUFF_CALL:
      handleWriteBuffCall(payload);
      return;
    case GEMDRIVE_CMD_FDATETIME_CALL:
      handleFdatetimeCall(payload);
      return;
    case GEMDRIVE_CMD_FSETDTA_CALL:
      handleFsetdtaCall(payload);
      return;
    case GEMDRIVE_CMD_DTA_EXIST_CALL:
      handleDtaExistCall(payload);
      return;
    case GEMDRIVE_CMD_PEXEC_CALL:
      handlePexecCall(payload);
      return;
    case GEMDRIVE_CMD_SAVE_EXEC_HEADER:
      handleSaveExecHeader(payload);
      return;
    case GEMDRIVE_CMD_SAVE_BASEPAGE:
      handleSaveBasepage(payload);
      return;
    case GEMDRIVE_CMD_FOPEN_CALL:
      handleFopenCall(payload);
      return;
    case GEMDRIVE_CMD_FCLOSE_CALL:
      handleFcloseCall(payload);
      return;
    case GEMDRIVE_CMD_FSEEK_CALL:
      handleFseekCall(payload);
      return;
    case GEMDRIVE_CMD_READ_BUFF_CALL:
      handleReadBuffCall(payload);
      return;
    case GEMDRIVE_CMD_FSFIRST_CALL:
      handleFsfirstCall(payload);
      return;
    case GEMDRIVE_CMD_FSNEXT_CALL:
      handleFsnextCall(payload);
      return;
    case GEMDRIVE_CMD_DTA_RELEASE_CALL:
      handleDtaReleaseCall(payload);
      return;
    case GEMDRIVE_CMD_VERIFY_MEMTOP: {
      uint32_t observed = TPROTO_GET_PAYLOAD_PARAM32(payload);
      DPRINTF(
          "GEMDRIVE VERIFY_MEMTOP: [F]irmware path read 0x%08lX from $436\n",
          (unsigned long)observed);
      return;
    }
    case GEMDRIVE_CMD_HELLO:
      break;
    default:
      return;
  }

  // ---- HELLO ----
  uint32_t screenBase = TPROTO_GET_PAYLOAD_PARAM32(payload);

  bool enabled = aconfigBool(ACONFIG_PARAM_GEMDRIVE_ENABLED, true);
  if (!enabled) {
    DPRINTF("GEMDRIVE disabled via aconfig; HELLO ignored.\n");
    return;
  }

  uint32_t relocOverride = aconfigInt(ACONFIG_PARAM_GEMDRIVE_RELOC_ADDR);
  uint32_t memtopOverride = aconfigInt(ACONFIG_PARAM_DEVOPS_MEMTOP);
  uint32_t defaultReloc = screenBase - GEMDRIVE_DEFAULT_OFFSET_BYTES;
  uint32_t effectiveReloc = (relocOverride != 0) ? relocOverride : defaultReloc;
  uint32_t effectiveMemtop =
      (memtopOverride != 0) ? memtopOverride : effectiveReloc;
  effectiveReloc &= ~0x3u;
  effectiveMemtop &= ~0x3u;

  uint32_t base = sharedBaseAddress();
  SET_SHARED_VAR(GEMDRIVE_SVAR_RELOC_ADDR, effectiveReloc, base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(GEMDRIVE_SVAR_MEMTOP, effectiveMemtop, base,
                 CHANDLER_SHARED_VARIABLES_OFFSET);

  char letter = resolveDriveLetter();
  uint8_t driveNumber = (uint8_t)(letter - 'A');
  gemdriveVectors.drive_letter = letter;
  gemdriveVectors.drive_number = driveNumber;
  publishDriveAndReentry(driveNumber, letter);

  DPRINTF(
      "GEMDRIVE HELLO: screen_base=0x%08lX -> reloc=0x%08lX (%s), "
      "memtop=0x%08lX (%s), drive=%c (#%u)\n",
      (unsigned long)screenBase, (unsigned long)effectiveReloc,
      (relocOverride != 0) ? "override" : "auto",
      (unsigned long)effectiveMemtop,
      (memtopOverride != 0) ? "override" : "auto",
      letter, (unsigned)driveNumber);

  // GEMDRIVE HELLO is the unambiguous "ST cold-booted" signal — the
  // m68k only sends it from gemdrive_init at CA_INIT bit 27. Notify
  // emul.c so it can re-fire DISPLAY_COMMAND_START_RUNNER when the
  // user pressed the ST's physical reset button (which never goes
  // through handle_runner_reset and would otherwise leave the m68k
  // stuck in the print loop with no relaunch ticker scheduled).
  emul_onGemdriveHello();
}

void gemdrive_init(void) {
  for (int i = 0; i < GEMDRIVE_MAX_OPEN_FILES; i++) fileTable[i].inUse = false;
  for (int i = 0; i < GEMDRIVE_MAX_DTAS; i++) dtaTable[i].inUse = false;
  chandler_addCB(gemdrive_command_cb);
  DPRINTF("GEMDRIVE callback registered (S3 phase 2).\n");
}
