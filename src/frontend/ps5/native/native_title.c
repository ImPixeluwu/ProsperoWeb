// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native title support (native_title.h).

#include "native_title.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int sceKernelDebugOutText(int channel, const char* text);
int sceKernelUsleep(uint32_t microseconds);
int sceSysmoduleLoadModule(uint16_t id);

void native_debug_printf(const char* fmt, ...) {
  char line[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  sceKernelDebugOutText(0, line);
}

// A title gets only its standard set of modules at start-up; the others
// are loaded by id before the first call, or the imported functions stay
// at address 0 (Stremio PS5, native/ps5_modules.c; the Kodi port's
// PS5ImeDialog.cpp).
int native_load_system_modules(char* report, size_t report_len) {
  static const struct {
    uint16_t id;
    const char* name;
  } kModules[] = {
      {0x0096, "libSceImeDialog"},
  };
  int failed = 0;
  size_t used = 0;
  if (report_len) {
    report[0] = 0;
  }
  for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++) {
    const int rc = sceSysmoduleLoadModule(kModules[i].id);
    failed += rc != 0;
    if (used < report_len) {
      const int n = snprintf(report + used, report_len - used, "%s%s (sysmodule %#x): %s %#x",
                             used ? "; " : "", kModules[i].name, (unsigned)kModules[i].id,
                             rc == 0 ? "loaded" : "FAILED", (unsigned)rc);
      used += n > 0 ? (size_t)n : 0;
    }
  }
  return failed;
}

// ---- libSceKeyboard: none ------------------------------------------------------
//
// libSceKeyboard (USB keyboards, which SDL opens) is not among a title's
// modules. These stand-ins report no keyboard, so SDL skips it (Stremio
// PS5, native/ps5_modules.c).

int sceKeyboardInit(void) { return 0; }

int sceKeyboardOpen(int user, int type, int index, void* param) {
  (void)user;
  (void)type;
  (void)index;
  (void)param;
  return -1;
}

int sceKeyboardReadState(int handle, void* state) {
  (void)handle;
  (void)state;
  return -1;
}

// ---- the end of main() ---------------------------------------------------------
//
// Returning from main() or calling exit() crashes a native title (the
// ps5-homebrew-ui runtime shims); app_crt.cpp calls this hook first. The
// title stays alive until the system closes it (the PS button, "Close
// Game").

__attribute__((noreturn)) void catchReturnFromMain(int status) {
  native_debug_printf("[prosperoweb] main returned %d; waiting for the system to close the "
                      "title\n",
                      status);
  fflush(NULL);
  for (;;) {
    sceKernelUsleep(1000000);
  }
}
