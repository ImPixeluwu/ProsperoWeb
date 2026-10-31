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
static int g_ime_dialog_loaded;

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
    if (kModules[i].id == 0x0096) {
      g_ime_dialog_loaded = rc == 0;
    }
    if (used < report_len) {
      const int n = snprintf(report + used, report_len - used, "%s%s (sysmodule %#x): %s %#x",
                             used ? "; " : "", kModules[i].name, (unsigned)kModules[i].id,
                             rc == 0 ? "loaded" : "FAILED", (unsigned)rc);
      used += n > 0 ? (size_t)n : 0;
    }
  }
  return failed;
}

// ---- libSceImeDialog when its module did not load --------------------------------
//
// SDL's PS5 video driver polls the keyboard dialog every frame. Without the
// module the import is address 0, so the link wraps the calls
// (src/frontend/ps5/CMakeLists.txt) and they report "no dialog" / fail
// unless the module loaded (syobon-action-ps5, src/ps5_compat.cpp, stubs
// sceImeDialogGetStatus the same way).

int __real_sceImeDialogGetStatus(void);
int __real_sceImeDialogInit(const void* param, void* extended);

int __wrap_sceImeDialogGetStatus(void) {
  return g_ime_dialog_loaded ? __real_sceImeDialogGetStatus() : 0;  // STATUS_NONE
}

int __wrap_sceImeDialogInit(const void* param, void* extended) {
  return g_ime_dialog_loaded ? __real_sceImeDialogInit(param, extended) : -1;
}

// ---- libSceKeyboard: none ------------------------------------------------------
//
// In a title sceKeyboardInit crashes (syobon-action-ps5, README "Port
// notes", verified on hardware), and libSceKeyboard is not among a title's
// modules. These stand-ins, linked in place of the imports, report no
// keyboard, which SDL copes with.

int sceKeyboardInit(void) { return -1; }

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
