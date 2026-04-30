/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "display_term.h"
#include "ff.h"
#include "gconfig.h"
#include "gemdrive.h"
#include "memfunc.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"

#define SLEEP_LOOP_MS 100

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Command handlers
static void cmdMenu(const char *arg);
static void cmdExit(const char *arg);
static void cmdFirmware(const char *arg);
static void cmdBooster(const char *arg);
static void cmdGemdriveFolder(const char *arg);
static void cmdGemdriveDrive(const char *arg);
static void cmdGemdriveRelocAddr(const char *arg);
static void cmdGemdriveMemtop(const char *arg);
static void cmdHiddenSettings(const char *arg);
static void cmdPrint(const char *arg);
static void cmdSave(const char *arg);
static void cmdErase(const char *arg);
static void cmdGet(const char *arg);
static void cmdPutInt(const char *arg);
static void cmdPutBool(const char *arg);
static void cmdPutString(const char *arg);

// Command table. Single-letter keys mirror md-drives-emulator. The hidden
// "?" entry exposes the raw settings commands (print/save/get/...).
static const Command commands[] = {
    {"m", cmdMenu},
    {"e", cmdExit},
    {"f", cmdFirmware},
    {"x", cmdBooster},
    {"o", cmdGemdriveFolder},
    {"d", cmdGemdriveDrive},
    {"r", cmdGemdriveRelocAddr},
    {"t", cmdGemdriveMemtop},
    {"?", cmdHiddenSettings},
    {"print", cmdPrint},
    {"save", cmdSave},
    {"erase", cmdErase},
    {"get", cmdGet},
    {"put_int", cmdPutInt},
    {"put_bool", cmdPutBool},
    {"put_str", cmdPutString},
};

// Number of commands in the table
static const size_t numCommands = sizeof(commands) / sizeof(commands[0]);

// Keep active loop or exit
static bool keepActive = true;
static bool menuScreenActive = false;

// Boot countdown — auto-launches GEMDRIVE on the Atari ST when it hits 0.
// Mirrors md-drives-emulator's behavior. Any key press halts it.
#define BOOT_COUNTDOWN_SECONDS 20
static int countdown = BOOT_COUNTDOWN_SECONDS;
static bool haltCountdown = false;
static absolute_time_t lastCountdownTick;

// Polling tick used as the network poll callback so command handling stays
// alive during multi-second WiFi operations.
static void __not_in_flash_func(emul_pollTick)(void) {
  chandler_loop();
  term_loop();
}

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// --- Helpers ported verbatim from md-drives-emulator/rp/src/emul.c -----

// Returns the last n chars of str, prefixed with ".." if truncated.
// Caller frees. NULL on bad input.
static char *__not_in_flash_func(right)(const char *str, int n) {
  if (str == NULL || n < 0) return NULL;
  int len = (int)strlen(str);
  if (n == 0) return strdup("");
  if (n >= len) return strdup(str);
  const char *suffix = str + len - n;
  char *result = malloc(n + 3);
  if (!result) return NULL;
  strcpy(result, "..");
  strncpy(result + 2, suffix, n);
  result[n + 2] = '\0';
  return result;
}

// "C".."Z" only; identical predicate to source's GEMDRIVE drive validator.
static bool __not_in_flash_func(isValidDrive)(const char *drive) {
  if (drive == NULL || drive[0] == '\0') {
    return false;
  }
  char c = (char)toupper((unsigned char)drive[0]);
  return (c >= 'C' && c <= 'Z');
}

// VT52 absolute cursor move (ESC Y row col, both biased by 0x20).
static inline void vt52Cursor(uint8_t row, uint8_t col) {
  char vt52Seq[5];
  vt52Seq[0] = '\x1B';
  vt52Seq[1] = 'Y';
  vt52Seq[2] = (char)(32 + row);
  vt52Seq[3] = (char)(32 + col);
  vt52Seq[4] = '\0';
  term_printString(vt52Seq);
}

// Title with reverse-video (ESC p / ESC q) — matches the source's
// "two font sizes / dim+bright" feel by inverting the title bar.
static void showTitle(void) {
  term_printString(
      "\x1B"
      "E"
      "\x1Bp"
      "DevOps Microfirmware - " RELEASE_VERSION "\n\x1Bq");
}

static void __not_in_flash_func(showCounter)(int cdown);

// Bottom-of-OLED info strip rendered with the smaller squeezed font —
// this is the "second font size" referenced by the source. Inverts a 1px
// strip the height of one terminal row.
static void drawSetupInfoLine(const char *message) {
  u8g2_SetDrawColor(display_getU8g2Ref(), 1);
  u8g2_DrawBox(display_getU8g2Ref(), 0,
               DISPLAY_HEIGHT - DISPLAY_TERM_CHAR_HEIGHT, DISPLAY_WIDTH,
               DISPLAY_TERM_CHAR_HEIGHT);
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_squeezed_b7_tr);
  u8g2_SetDrawColor(display_getU8g2Ref(), 0);
  u8g2_DrawStr(display_getU8g2Ref(), 0, DISPLAY_HEIGHT - 1,
               (message != NULL) ? message : "");
  u8g2_SetDrawColor(display_getU8g2Ref(), 1);
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_amstrad_cpc_extended_8f);
}

static void refreshSetupInfoLine(void) {
  if (haltCountdown) {
    drawSetupInfoLine("Countdown stopped. Press [E] or [X] to continue.");
  } else {
    showCounter(countdown);
  }
}

// --- Directory navigation pager (ported from md-drives-emulator) ------

#define NAV_LINES_PER_PAGE 16
#define NAV_LINES_PER_PAGE_OFFSET 4
enum navStatus {
  NAV_DIR_ERROR = -1,
  NAV_DIR_FIRST_TIME_OK = 0,
  NAV_DIR_NEXT_TIME_OK = 1,
  NAV_DIR_SELECTED = 2,
  NAV_DIR_CANCEL = 3,
};

typedef struct {
  uint16_t count;
  uint16_t selected;
  uint16_t page;
  char entries[MAX_ENTRIES_DIR][MAX_FILENAME_LENGTH + 1];
  char folderPath[MAX_FILENAME_LENGTH + 1];
  char topDir[MAX_FILENAME_LENGTH + 1];
} DirNavigation;

static DirNavigation navStateStorage;
static DirNavigation *navState = &navStateStorage;

// Filter: directories only — we list folders, never files. Hidden dotfiles
// are skipped. Mirrors the spirit of source's floppiesFilter for our case.
static bool __not_in_flash_func(foldersOnlyFilter)(const char *name,
                                                   BYTE attr) {
  if (name[0] == '.') {
    return false;
  }
  return (attr & AM_DIR) != 0;
}

// Remove last path component (".." navigation).
static void __not_in_flash_func(pathUp)(void) {
  char temp[MAX_FILENAME_LENGTH + 1];
  char *segments[MAX_ENTRIES_DIR];
  int sp = 0;

  strncpy(temp, navState->folderPath, sizeof(temp));
  temp[sizeof(temp) - 1] = '\0';

  char *token = strtok(temp, "/");
  while (token) {
    if (strcmp(token, "..") == 0) {
      if (sp > 0) sp--;
    } else if (strcmp(token, "") != 0) {
      segments[sp++] = token;
    }
    token = strtok(NULL, "/");
  }

  if (sp == 0) {
    strcpy(navState->folderPath, "/");
  } else {
    char newPath[MAX_FILENAME_LENGTH + 1] = "";
    for (int i = 0; i < sp; ++i) {
      strlcat(newPath, "/", sizeof(newPath));
      strlcat(newPath, segments[i], sizeof(newPath));
    }
    if (newPath[0] != '/') {
      char tmp[MAX_FILENAME_LENGTH + 1];
      snprintf(tmp, sizeof(tmp), "/%s", newPath);
      strncpy(newPath, tmp, sizeof(newPath));
    }
    strncpy(navState->folderPath, newPath, sizeof(navState->folderPath));
    navState->folderPath[sizeof(navState->folderPath) - 1] = '\0';
  }
}

static void drawPage(uint16_t top_offset) {
  uint16_t start = navState->page * NAV_LINES_PER_PAGE;
  uint16_t end = start + NAV_LINES_PER_PAGE;
  if (end > navState->count) {
    end = navState->count;
  }

  for (uint16_t i = start; i < end; i++) {
    uint8_t row = i - start;
    vt52Cursor(row + top_offset, 0);
    term_printString(i == navState->selected ? ">" : " ");
    char buffer[TERM_SCREEN_SIZE_X + 1];
    snprintf(buffer, sizeof(buffer), " %-*s", TERM_SCREEN_SIZE_X - 2,
             navState->entries[i]);
    term_printString(buffer);
  }

  vt52Cursor(TERM_SCREEN_SIZE_Y - 3, 0);
  char infoBuffer[128];
  int totalPages =
      (navState->count + NAV_LINES_PER_PAGE - 1) / NAV_LINES_PER_PAGE;
  sprintf(infoBuffer, "Page %d/%d\n", navState->page + 1, totalPages);
  term_printString(infoBuffer);
  term_printString("Use cursor keys and RETURN to navigate.\n");
  term_printString("SPACE to confirm selection. ESC to exit");
}

static enum navStatus __not_in_flash_func(navigate_directory)(
    bool first_time, bool dirs_only, char key, EntryFilterFn filter_fn,
    char top_folder[MAX_FILENAME_LENGTH + 1]) {
  enum navStatus status = NAV_DIR_ERROR;
  if (first_time) {
    DPRINTF("First time loading directory.\n");
    navState->count = 0;
    navState->selected = 0;
    navState->page = 0;
    if (top_folder != NULL) {
      strncpy(navState->topDir, top_folder, sizeof(navState->topDir));
      navState->topDir[sizeof(navState->topDir) - 1] = '\0';
    } else {
      strncpy(navState->topDir, "/", sizeof(navState->topDir));
      navState->topDir[sizeof(navState->topDir) - 1] = '\0';
    }
    memset(navState->entries, 0, sizeof(navState->entries));
    FRESULT result = sdcard_loadDirectory(
        (strlen(navState->folderPath) == 0) ? "/" : navState->folderPath,
        navState->entries, &navState->count, &navState->selected,
        &navState->page, dirs_only, filter_fn, navState->topDir);
    if (result != FR_OK) {
      term_printString("Error loading directory.\n");
    } else {
      status = NAV_DIR_FIRST_TIME_OK;
    }
  } else {
    DPRINTF("Next times loading directory.\n");
    status = NAV_DIR_NEXT_TIME_OK;
    int totalPages =
        (navState->count + NAV_LINES_PER_PAGE - 1) / NAV_LINES_PER_PAGE;
    switch (key) {
      case TERM_KEYBOARD_KEY_UP:
        if (navState->selected > 0) {
          navState->selected--;
        }
        break;
      case TERM_KEYBOARD_KEY_DOWN:
        if ((navState->selected < navState->count - 1) &&
            (navState->selected % NAV_LINES_PER_PAGE <
             NAV_LINES_PER_PAGE - 1)) {
          navState->selected++;
        }
        break;
      case TERM_KEYBOARD_KEY_LEFT:
        if (navState->page > 0) {
          navState->page--;
          navState->selected = navState->page * NAV_LINES_PER_PAGE;
        }
        break;
      case TERM_KEYBOARD_KEY_RIGHT:
        if (navState->page < (totalPages - 1)) {
          navState->page++;
          navState->selected = navState->page * NAV_LINES_PER_PAGE;
        }
        break;
      case '\r':
      case '\n': {
        if ((navState->entries[navState->selected]
                              [strlen(navState->entries[navState->selected]) -
                               1] == '/') ||
            (strcmp(navState->entries[navState->selected], "..") == 0)) {
          navState->count = 0;
          navState->page = 0;
          char newFolderPath[MAX_FILENAME_LENGTH + 1];
          size_t len = strlen(navState->folderPath);
          if (len > 0 && navState->folderPath[len - 1] != '/') {
            snprintf(newFolderPath, sizeof(newFolderPath), "%s/%s",
                     navState->folderPath,
                     navState->entries[navState->selected]);
          } else {
            snprintf(newFolderPath, sizeof(newFolderPath), "%s%s",
                     navState->folderPath,
                     navState->entries[navState->selected]);
          }
          strncpy(navState->folderPath, newFolderPath, sizeof(newFolderPath));
          pathUp();
          memset(navState->entries, 0, sizeof(navState->entries));
          navState->selected = 0;
          FRESULT result = sdcard_loadDirectory(
              navState->folderPath, navState->entries, &navState->count,
              &navState->selected, &navState->page, dirs_only, filter_fn,
              navState->topDir);
          if (result != FR_OK) {
            term_printString("Error loading directory.\n");
            status = NAV_DIR_ERROR;
          }
        }
        break;
      }
      case ' ': {
        status = NAV_DIR_SELECTED;
        break;
      }
      default:
        if (key >= 'a' && key <= 'z') {
          key = key - 'a' + 'A';
        }
        if (key >= 'A' && key <= 'Z') {
          status = key;
        }
        break;
    }
  }
  return status;
}

// Builds the menu — single GEMDRIVE block + bottom navigation strip.
// Layout follows the source's menu(): vt52Cursor positions, the F[o]lder/
// [D]rive labels, and the bottom "[E]xit / [X] Return to Booster" line.
static void __not_in_flash_func(menu)(void) {
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  menuScreenActive = true;

  showTitle();

  // Folder + drive read straight from aconfig, like source/md-drives.
  vt52Cursor(2, 0);
  term_printString("GEMDRIVE\n");

  SettingsConfigEntry *gemDriveFolder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
  const char *folderValue =
      (gemDriveFolder != NULL && gemDriveFolder->value[0] != '\0')
          ? gemDriveFolder->value
          : "/devops";
  char *folderTail = right(folderValue, 24);
  term_printString("  F[o]lder      : ");
  term_printString((folderTail != NULL) ? folderTail : folderValue);
  if (folderTail != NULL) free(folderTail);

  SettingsConfigEntry *gemDriveDrive =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_DRIVE);
  const char *driveValue =
      (gemDriveDrive != NULL && gemDriveDrive->value[0] != '\0')
          ? gemDriveDrive->value
          : "C";
  term_printString("\n  [D]rive       : ");
  term_printString(driveValue);
  term_printString(":");

  // Reloc / memtop overrides — 0 means "auto" (default = screen_base - 8 KB).
  SettingsConfigEntry *relocEntry = settings_find_entry(
      aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_RELOC_ADDR);
  int relocAddr = (relocEntry != NULL) ? atoi(relocEntry->value) : 0;
  char relocLine[48];
  if (relocAddr == 0) {
    snprintf(relocLine, sizeof(relocLine),
             "\n  [R]eloc addr  : auto (screen-8KB)");
  } else {
    snprintf(relocLine, sizeof(relocLine), "\n  [R]eloc addr  : 0x%06X",
             (unsigned)relocAddr);
  }
  term_printString(relocLine);

  SettingsConfigEntry *memtopEntry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_DEVOPS_MEMTOP);
  int memtop = (memtopEntry != NULL) ? atoi(memtopEntry->value) : 0;
  char memtopLine[48];
  if (memtop == 0) {
    snprintf(memtopLine, sizeof(memtopLine),
             "\n  Mem[t]op      : auto (matches reloc)");
  } else {
    snprintf(memtopLine, sizeof(memtopLine), "\n  Mem[t]op      : 0x%06X",
             (unsigned)memtop);
  }
  term_printString(memtopLine);
  term_printString("\n");

  vt52Cursor(TERM_SCREEN_SIZE_Y - 2, 0);
  term_printString("[E]xit (launch)   [X] Return to Booster");

  vt52Cursor(TERM_SCREEN_SIZE_Y - 1, 0);
  term_printString("Select an option: ");
  refreshSetupInfoLine();
}

static void __not_in_flash_func(showCounter)(int cdown) {
  char msg[64];
  if (cdown > 0) {
    sprintf(msg, "Boot will continue in %d seconds...", cdown);
  } else {
    showTitle();
    sprintf(msg, "Booting... Please wait...               ");
  }
  drawSetupInfoLine(msg);
}

// --- Command handlers (single-key dispatch) ----------------------------

void cmdMenu(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menu();
}

// [E]xit doubles as the firmware-launch shortcut now: per the user's
// directive, leaving the menu drops straight into GEMDRIVE on the Atari ST
// (same path as [F]).
void cmdExit(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  showTitle();
  term_printString("\n\n");
  term_printString("Launching DevOps on the Atari ST...\n");
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
}

void cmdFirmware(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  term_printString("Launching DevOps on the Atari ST...\n");
  // CMD_START is the cartridge sentinel that GEMDRIVE polls during
  // pre_auto; receiving it makes the m68k jump into the GEMDRIVE blob.
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
}

void cmdBooster(const char *arg) {
  (void)arg;
  showTitle();
  term_printString("\n\n");
  term_printString("Launching Booster app...\n");
  term_printString("The computer will boot shortly...\n\n");
  term_printString("If it doesn't boot, power it on and off.\n");
  haltCountdown = true;
  menuScreenActive = false;
  resetDeviceAtBoot = false;  // Jump to the booster app
  keepActive = false;
}

// Folder picker — directly ported from md-drives-emulator's
// cmdGemdriveFolder. First press of [O] paints the SD card directory at
// the currently configured folder; subsequent keystrokes (arrows / RETURN
// / SPACE) come back through TERM_COMMAND_LEVEL_COMMAND_SINGLE_KEY_REENTRY
// and drive navigate_directory.
void __not_in_flash_func(cmdGemdriveFolder)(const char *arg) {
  haltCountdown = true;
  enum navStatus status = NAV_DIR_ERROR;
  switch (term_getCommandLevel()) {
    case TERM_COMMAND_LEVEL_SINGLE_KEY: {
      DPRINTF("Folder picker entering reentry mode.\n");
      SettingsConfigEntry *gemDriveFolder = settings_find_entry(
          aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
      const char *seed =
          (gemDriveFolder != NULL && gemDriveFolder->value[0] != '\0')
              ? gemDriveFolder->value
              : "/";
      strncpy(navState->folderPath, seed, sizeof(navState->folderPath));
      navState->folderPath[sizeof(navState->folderPath) - 1] = '\0';
      term_setCommandLevel(TERM_COMMAND_LEVEL_COMMAND_SINGLE_KEY_REENTRY);
      status = navigate_directory(true, true, '\0', foldersOnlyFilter, NULL);
      break;
    }
    case TERM_COMMAND_LEVEL_COMMAND_SINGLE_KEY_REENTRY: {
      DPRINTF("Folder picker key: %d\n", arg[0]);
      char key = arg[0];
      // ESC cancels the picker and returns to the menu.
      if (key == 27) {
        term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
        menu();
        return;
      }
      status =
          navigate_directory(false, true, key, foldersOnlyFilter, NULL);
      break;
    }
    default:
      break;
  }
  switch (status) {
    case NAV_DIR_FIRST_TIME_OK:
    case NAV_DIR_NEXT_TIME_OK: {
      showTitle();
      term_printString("\nFolder in the micro SD card: ");
      term_printString(navState->folderPath);
      drawPage(NAV_LINES_PER_PAGE_OFFSET);
      break;
    }
    case NAV_DIR_SELECTED: {
      settings_put_string(aconfig_getContext(),
                          ACONFIG_PARAM_GEMDRIVE_FOLDER,
                          navState->folderPath);
      settings_save(aconfig_getContext(), true);
      term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
      menu();
      break;
    }
    default:
      break;
  }
}

// Drive picker — verbatim adaptation of source's cmdGemdriveDrive.
void cmdGemdriveDrive(const char *arg) {
  haltCountdown = true;
  if (term_getCommandLevel() == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    showTitle();
    term_printString("\n\n");
    term_printString("Enter the drive (C to Z):\n\n> ");
    term_setCommandLevel(TERM_COMMAND_LEVEL_DATA_INPUT);
    return;
  }
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  const char *input = (arg != NULL) ? arg : "";
  if (!isValidDrive(input)) {
    term_printString("Invalid drive. Press SPACE to continue...\n");
    return;
  }
  char driveBuffer[2] = {(char)toupper((unsigned char)input[0]), '\0'};
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_DRIVE,
                      driveBuffer);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Reloc-address picker. Same single-key → data-input pattern as drive.
// Accepts hex (0x...) or decimal. 0 / "auto" / empty restores the default.
void cmdGemdriveRelocAddr(const char *arg) {
  haltCountdown = true;
  if (term_getCommandLevel() == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    showTitle();
    term_printString("\n\n");
    term_printString("Enter the relocation address (hex 0x...):\n");
    term_printString("Use 0 or empty for auto (screen-8KB).\n\n> ");
    term_setCommandLevel(TERM_COMMAND_LEVEL_DATA_INPUT);
    return;
  }
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  const char *input = (arg != NULL) ? arg : "";
  // Trim leading spaces.
  while (*input == ' ' || *input == '\t') input++;
  long value = 0;
  if ((input[0] == '\0') || (strcasecmp(input, "auto") == 0)) {
    value = 0;
  } else {
    char *end = NULL;
    int base = (input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
                   ? 16
                   : 10;
    value = strtol(input, &end, base);
    if ((end == input) || (value < 0)) {
      term_printString("Invalid address. Press SPACE to continue...\n");
      return;
    }
  }
  settings_put_integer(aconfig_getContext(),
                       ACONFIG_PARAM_GEMDRIVE_RELOC_ADDR, (int)value);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Memtop picker. Same shape as the reloc one. 0 mirrors reloc by default.
void cmdGemdriveMemtop(const char *arg) {
  haltCountdown = true;
  if (term_getCommandLevel() == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    showTitle();
    term_printString("\n\n");
    term_printString("Enter the _memtop value (hex 0x...):\n");
    term_printString("Use 0 or empty to follow the reloc address.\n\n> ");
    term_setCommandLevel(TERM_COMMAND_LEVEL_DATA_INPUT);
    return;
  }
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  const char *input = (arg != NULL) ? arg : "";
  while (*input == ' ' || *input == '\t') input++;
  long value = 0;
  if ((input[0] == '\0') || (strcasecmp(input, "auto") == 0)) {
    value = 0;
  } else {
    char *end = NULL;
    int base = (input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
                   ? 16
                   : 10;
    value = strtol(input, &end, base);
    if ((end == input) || (value < 0)) {
      term_printString("Invalid memtop. Press SPACE to continue...\n");
      return;
    }
  }
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_DEVOPS_MEMTOP,
                       (int)value);
  settings_save(aconfig_getContext(), true);
  menu();
}

// Hidden command list — switches the term to line-based COMMAND_INPUT so
// the user can type 'print', 'save', 'get key', etc.
void cmdHiddenSettings(const char *arg) {
  (void)arg;
  haltCountdown = true;
  menuScreenActive = false;
  showTitle();
  term_printString(
      "\n\n"
      "Available settings commands:\n"
      "  print   - Show settings\n"
      "  save    - Save settings\n"
      "  erase   - Erase settings\n"
      "  get     - Get setting (requires key)\n"
      "  put_int - Set integer (key and value)\n"
      "  put_bool- Set boolean (key and value)\n"
      "  put_str - Set string (key and value)\n\n"
      "Enter command. Type 'm' to return > ");
  term_setCommandLevel(TERM_COMMAND_LEVEL_COMMAND_INPUT);
}

void cmdPrint(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPrint(arg);
}

void cmdSave(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdSave(arg);
  // Cartridge-side state (GEMDRIVE relocation, _memtop patch, etc.) is
  // applied by gemdrive_init at CA_INIT time, so a saved aconfig change
  // is invisible until the m68k re-runs CA_INIT. Issue an automatic ST
  // reset so the change takes effect immediately.
  term_printString("Resetting Atari ST to apply changes...\n");
  display_refresh();
  sleep_ms(300);
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
}

void cmdErase(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdErase(arg);
}

void cmdGet(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdGet(arg);
}

void cmdPutInt(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPutInt(arg);
}

void cmdPutBool(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPutBool(arg);
}

void cmdPutString(const char *arg) {
  haltCountdown = true;
  menuScreenActive = false;
  term_cmdPutString(arg);
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString("Configuring network... please wait...\n");

  display_refresh();
}

void failure(const char *message) {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString(message);

  display_refresh();
}

static void init(void) {
  // Set the command table
  term_setCommands(commands, numCommands);

  // Clear the screen
  term_clearScreen();

  // Display the menu
  menu();

  // Example 1: Move the cursor up one line.
  // VT52 sequence: ESC A (moves cursor up)
  // The escape sequence "\x1BA" will move the cursor up one line.
  // term_printString("\x1B" "A");
  // After moving up, print text that overwrites part of the previous line.
  // term_printString("Line 2 (modified by ESC A)\n");

  // Example 2: Move the cursor right one character.
  // VT52 sequence: ESC C (moves cursor right)
  // term_printString("\x1B" "C");
  // term_printString(" <-- Moved right with ESC C\n");

  // Example 3: Direct cursor addressing.
  // VT52 direct addressing uses ESC Y <row> <col>, where:
  //   row_char = row + 0x20, col_char = col + 0x20.
  // For instance, to move the cursor to row 0, column 10:
  //   row: 0 -> 0x20 (' ')
  //   col: 10 -> 0x20 + 10 = 0x2A ('*')
  // term_printString("\x1B" "Y" "\x20" "\x2A");
  // term_printString("Text at row 0, column 10 via ESC Y\n");

  // term_printString("\x1B" "Y" "\x2A" "\x20");

  display_refresh();
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(term_command_cb);

  // Register GEMDRIVE before the cartridge boots — the m68k issues
  // CMD_GEMDRIVE_HELLO from CA_INIT (bit 27, after GEMDOS init), which
  // happens once the ST is powered on. The callback computes the
  // effective relocation address / _memtop value from aconfig overrides
  // (or screen_base - 8 KB by default) and publishes them via shared
  // variables before send_sync's random-token ack returns to the m68k.
  gemdrive_init();

  // After this point, the remote computer can execute the code

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.
  // If you want to use a custom app in the remote computer, you can do it.
  // But it's easier to debug and code in the rp2040

  // Initialize the display
  display_setupU8g2();

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues and reports SD status in the terminal menu.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/test";  // MODIFY THIS TO YOUR FOLDER NAME
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
    // Also ensure the GEMDRIVE folder exists. When the user boots a
    // fresh card, the directory configured by GEMDRIVE_FOLDER (default
    // "/devops") is missing, and Fsfirst / Fopen fail with FR_NO_PATH
    // even though the SD itself is mounted. Mirrors what the
    // md-drives-emulator firmware does at boot.
    SettingsConfigEntry *gemdriveFolder = settings_find_entry(
        aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
    const char *gemdriveFolderName =
        (gemdriveFolder != NULL) ? gemdriveFolder->value : "/devops";
    sdcard_status_t gemdriveStatus = sdcard_ensureFolder(gemdriveFolderName);
    if (gemdriveStatus != SDCARD_INIT_OK) {
      DPRINTF("GEMDRIVE folder '%s' could not be ensured (status %i)\n",
              gemdriveFolderName, gemdriveStatus);
    } else {
      DPRINTF("GEMDRIVE folder '%s' ready.\n", gemdriveFolderName);
    }
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // Pre-init the stuff
  // In this example it only prints the please wait message, but can be used as
  // a place to put other code that needs to be run before the network is
  // initialized
  preinit();

  // 6. Init the network, if needed
  // It's always a good idea to wait for the network to be ready
  // Get the WiFi mode from the settings
  // If you are developing code that does not use the network, you can
  // comment this section
  // It's important to note that the network parameters are taken from the
  // global configuration of the Booster app. The network parameters are
  // ready only for the microfirmware apps.
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  wifi_mode_t wifiModeValue = WIFI_MODE_STA;
  if (wifiMode == NULL) {
    DPRINTF("No WiFi mode found in the settings. No initializing.\n");
  } else {
    wifiModeValue = (wifi_mode_t)atoi(wifiMode->value);
    if (wifiModeValue != WIFI_MODE_AP) {
      DPRINTF("WiFi mode is STA\n");
      wifiModeValue = WIFI_MODE_STA;
      int err = network_wifiInit(wifiModeValue);
      if (err != 0) {
        DPRINTF("Error initializing the network: %i. No initializing.\n", err);
      } else {
        // Drain commands and run the terminal loop during WiFi polling so
        // commands sent during the (potentially multi-second) connect don't
        // pile up in the ROM3 ring.
        network_setPollingCallback(emul_pollTick);
        // Connect to the WiFi network
        int maxAttempts = 3;  // or any other number defined elsewhere
        int attempt = 0;
        err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;

        while ((attempt < maxAttempts) &&
               (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
          err = network_wifiStaConnect();
          attempt++;

          if ((err > 0) && (err < NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
            DPRINTF("Error connecting to the WiFi network: %i\n", err);
          }
        }

        if (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT) {
          DPRINTF("Timeout connecting to the WiFi network after %d attempts\n",
                  maxAttempts);
          // Optionally, return an error code here.
        }
        network_setPollingCallback(NULL);
      }
    } else {
      DPRINTF("WiFi mode is AP. No initializing.\n");
    }
  }

  // 7. Configure the SELECT button so menu status can show it immediately.
  select_configure();

  // 8. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init();

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to exit.
  // For testing purposes, this app only shows commands to manage the settings
  DPRINTF("Start the app loop here\n");
  lastCountdownTick = get_absolute_time();
  while (getKeepActive()) {
#if PICO_CYW43_ARCH_POLL
    network_safePoll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(SLEEP_LOOP_MS));
#else
    sleep_ms(SLEEP_LOOP_MS);
#endif
    // Drain the ROM3 command ring → dispatch to registered callbacks.
    chandler_loop();

    // Run the terminal foreground (consume the published command, render
    // output, etc.).
    term_loop();

    // Any keystroke (visible in the terminal input buffer before Enter is
    // pressed) halts the autoboot countdown — matches md-drives-emulator.
    if (!haltCountdown) {
      char *input = term_getInputBuffer();
      if ((input != NULL) && (input[0] != '\0')) {
        haltCountdown = true;
      }
    }

    if (!haltCountdown && menuScreenActive) {
      absolute_time_t now = get_absolute_time();
      if (absolute_time_diff_us(lastCountdownTick, now) >= 1000000) {
        lastCountdownTick = now;
        countdown--;
        showCounter(countdown);
        display_refresh();
        if (countdown <= 0) {
          haltCountdown = true;
          // Autoboot expired — launch DevOps on the Atari ST. Same path
          // as pressing [F].
          cmdFirmware(NULL);
        }
      }
    }
  }

  // 10. Send RESET computer command
  // Ok, so we are done with the setup but we want to reset the computer to
  // reboot in the same microfirmware app or start the booster app

  sleep_ms(SLEEP_LOOP_MS);
  // We must reset the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(SLEEP_LOOP_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Set emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         APP_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Jump to the booster app
    DPRINTF("Jumping to the booster app...\n");
    reset_jump_to_booster();
  }
}
