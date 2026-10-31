// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_ime.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <utility>

#include "prosperoweb/core/utf16.h"
#include "ps5_log.h"

// --- libSceImeDialog / libSceUserService declarations ----------------------
//
// The payload SDK ships the stub libraries but no headers. The structures and
// prototypes below are adapted from the SDL2 PS5 port
// (src/video/ps5/SDL_ps5keyboard.c):
//
//   Simple DirectMedia Layer
//   Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>
//
//   This software is provided 'as-is', without any express or implied
//   warranty.  In no event will the authors be held liable for any damages
//   arising from the use of this software.
//
//   Permission is granted to anyone to use this software for any purpose,
//   including commercial applications, and to alter it and redistribute it
//   freely, subject to the following restrictions:
//
//   1. The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software
//      in a product, an acknowledgment in the product documentation would be
//      appreciated but is not required.
//   2. Altered source versions must be plainly marked as such, and must not be
//      misrepresented as being the original software.
//   3. This notice may not be removed or altered from any source distribution.
//
// Altered: C++ enums with fixed underlying types, wchar_t replaced by
// char16_t (the console's wchar_t is 16 bits; the SDK's is too, see the
// static_assert), the option bits added (PS4 libSceIme values, unverified on
// the PS5) and layout assertions.

namespace {

enum SceImeDialogStatus : int {
  SCE_IME_DIALOG_STATUS_NONE = 0,
  SCE_IME_DIALOG_STATUS_RUNNING = 1,
  SCE_IME_DIALOG_STATUS_FINISHED = 2,
};

enum SceImeType : int {
  SCE_IME_TYPE_DEFAULT = 0,
  SCE_IME_TYPE_BASIC_LATIN = 1,
  SCE_IME_TYPE_URL = 2,
  SCE_IME_TYPE_MAIL = 3,
  SCE_IME_TYPE_NUMBER = 4,
};

enum SceImeEnterLabel : int {
  SCE_IME_ENTER_LABEL_DEFAULT = 0,
  SCE_IME_ENTER_LABEL_SEND = 1,
  SCE_IME_ENTER_LABEL_SEARCH = 2,
  SCE_IME_ENTER_LABEL_GO = 3,
};

enum SceImeInputMethod : int { SCE_IME_INPUT_METHOD_DEFAULT = 0 };

enum SceImeHorizontalAlignment : int {
  SCE_IME_HALIGN_LEFT = 0,
  SCE_IME_HALIGN_CENTER = 1,
  SCE_IME_HALIGN_RIGHT = 2,
};

enum SceImeVerticalAlignment : int {
  SCE_IME_VALIGN_TOP = 0,
  SCE_IME_VALIGN_CENTER = 1,
  SCE_IME_VALIGN_BOTTOM = 2,
};

constexpr std::uint32_t SCE_IME_OPTION_DEFAULT = 0x00;
constexpr std::uint32_t SCE_IME_OPTION_NO_AUTO_CAPITALIZATION = 0x02;
constexpr std::uint32_t SCE_IME_OPTION_PASSWORD = 0x04;

enum SceImeDialogEndStatus : int {
  SCE_IME_DIALOG_END_STATUS_OK = 0,
  SCE_IME_DIALOG_END_STATUS_USER_CANCELED = 1,
  SCE_IME_DIALOG_END_STATUS_ABORTED = 2,
};

using SceImeTextFilter = int (*)(char16_t*, std::uint32_t*, const char16_t*, std::uint32_t);

struct SceImeDialogParam {
  int userId;
  SceImeType type;
  std::uint64_t supportedLanguages;
  SceImeEnterLabel enterLabel;
  SceImeInputMethod inputMethod;
  SceImeTextFilter filter;
  std::uint32_t option;
  std::uint32_t maxTextLength;
  char16_t* inputTextBuffer;
  float posx;
  float posy;
  SceImeHorizontalAlignment horizontalAlignment;
  SceImeVerticalAlignment verticalAlignment;
  const char16_t* placeholder;
  const char16_t* title;
  std::int8_t reserved[16];
};

struct SceImeDialogResult {
  SceImeDialogEndStatus endstatus;
  std::int8_t reserved[12];
};

static_assert(sizeof(char16_t) == 2);
static_assert(sizeof(SceImeDialogParam) == 0x60, "PS4/PS5 SceImeDialogParam is 96 bytes");
static_assert(offsetof(SceImeDialogParam, filter) == 24);
static_assert(offsetof(SceImeDialogParam, inputTextBuffer) == 40);
static_assert(offsetof(SceImeDialogParam, posx) == 48);
static_assert(offsetof(SceImeDialogParam, placeholder) == 64);
static_assert(offsetof(SceImeDialogParam, title) == 72);
static_assert(sizeof(SceImeDialogResult) == 16);

}  // namespace

extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int* user_id);
int sceUserServiceGetInitialUser(int* user_id);

int sceImeDialogInit(const SceImeDialogParam* param, void* extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(SceImeDialogResult* result);
int sceImeDialogTerm(void);
}

namespace prosperoweb::frontend {

namespace {

// sceUserServiceInitialize() when SDL (PS5_Keyboard_Init) already did it.
constexpr std::int32_t kUserServiceAlreadyInitialized = static_cast<std::int32_t>(0x80960003u);
// After a successful sceImeDialogInit() the status should be RUNNING; give
// up on a dialog still reporting NONE after this many polls (~1 s).
constexpr int kStartPolls = 60;

const char* status_name(int status) {
  switch (status) {
    case SCE_IME_DIALOG_STATUS_NONE: return "none";
    case SCE_IME_DIALOG_STATUS_RUNNING: return "running";
    case SCE_IME_DIALOG_STATUS_FINISHED: return "finished";
    default: return "unknown";
  }
}

void fill_param(SceImeDialogParam& p, int user_id, shell::TextInputKind kind,
                std::uint32_t max_length, char16_t* buffer, const char16_t* title) {
  using shell::TextInputKind;
  p = SceImeDialogParam{};
  p.userId = user_id;
  switch (kind) {
    case TextInputKind::Url: p.type = SCE_IME_TYPE_URL; break;
    case TextInputKind::Email: p.type = SCE_IME_TYPE_MAIL; break;
    case TextInputKind::Number: p.type = SCE_IME_TYPE_NUMBER; break;
    default: p.type = SCE_IME_TYPE_DEFAULT; break;
  }
  p.supportedLanguages = 0;  // ignored without SCE_IME_OPTION_LANGUAGES
  p.enterLabel = kind == TextInputKind::Url      ? SCE_IME_ENTER_LABEL_GO
                 : kind == TextInputKind::Search ? SCE_IME_ENTER_LABEL_SEARCH
                                                 : SCE_IME_ENTER_LABEL_DEFAULT;
  p.inputMethod = SCE_IME_INPUT_METHOD_DEFAULT;
  p.filter = nullptr;
  p.option = kind == TextInputKind::Password                               ? SCE_IME_OPTION_PASSWORD
             : (kind == TextInputKind::Url || kind == TextInputKind::Email) ? SCE_IME_OPTION_NO_AUTO_CAPITALIZATION
                                                                           : SCE_IME_OPTION_DEFAULT;
  p.maxTextLength = max_length;
  p.inputTextBuffer = buffer;
  p.posx = 1920.0f / 2.0f;  // 1080p panel coordinates
  p.posy = 1080.0f / 2.0f;
  p.horizontalAlignment = SCE_IME_HALIGN_CENTER;
  p.verticalAlignment = SCE_IME_VALIGN_CENTER;
  p.placeholder = nullptr;
  p.title = title;
}

// The parameters SDL's SDL_StartTextInput() uses, known to open a dialog
// from a homebrew app: everything zero except the user, buffer and length.
void make_plain(SceImeDialogParam& p) {
  p.type = SCE_IME_TYPE_DEFAULT;
  p.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;
  p.option = SCE_IME_OPTION_DEFAULT;
  p.posx = 0.0f;
  p.posy = 0.0f;
  p.horizontalAlignment = SCE_IME_HALIGN_LEFT;
  p.verticalAlignment = SCE_IME_VALIGN_TOP;
}

}  // namespace

Ps5SystemIme::~Ps5SystemIme() { term(); }

std::int32_t Ps5SystemIme::ensure_user() {
  if (user_ready_) {
    return 0;
  }
  int rc = sceUserServiceInitialize(nullptr);
  if (rc != 0 && rc != kUserServiceAlreadyInitialized) {
    log_printf("ime: sceUserServiceInitialize error 0x%08x (continuing)", static_cast<unsigned>(rc));
  }
  int id = -1;
  rc = sceUserServiceGetForegroundUser(&id);
  const char* source = "foreground";
  if (rc != 0 || id < 0) {
    log_printf("ime: sceUserServiceGetForegroundUser error 0x%08x", static_cast<unsigned>(rc));
    rc = sceUserServiceGetInitialUser(&id);
    source = "initial";
  }
  if (rc != 0) {
    log_printf("ime: error 0x%08x (sceUserServiceGetInitialUser)", static_cast<unsigned>(rc));
    return rc;
  }
  user_id_ = id;
  user_ready_ = true;
  log_printf("ime: user id 0x%08x (%s user)", static_cast<unsigned>(id), source);
  return 0;
}

std::int32_t Ps5SystemIme::open(const std::string& title_utf8, const std::string& initial_utf8,
                                std::size_t max_length, shell::TextInputKind kind) {
  if (open_) {
    log_line("ime: open while a dialog is up, closing it first");
    term();
  }
  if (const std::int32_t rc = ensure_user(); rc != 0) {
    return rc;
  }
  const int stale = sceImeDialogGetStatus();
  if (stale != SCE_IME_DIALOG_STATUS_NONE) {
    // E.g. a dialog opened by SDL_StartTextInput(); only one can exist.
    log_printf("ime: status %s (%d) before open, terminating it", status_name(stale), stale);
    sceImeDialogTerm();
  }

  const std::size_t max_len = std::clamp<std::size_t>(max_length, 1, kMaxTextLength);
  const std::u16string initial = core::utf16_truncate(core::utf8_to_utf16(initial_utf8), max_len);
  buffer_.assign(kMaxTextLength + 1, u'\0');
  std::copy(initial.begin(), initial.end(), buffer_.begin());
  title_ = core::utf16_truncate(core::utf8_to_utf16(title_utf8), kMaxTitleLength);

  log_printf("ime: open title=\"%s\" kind=%s len=%zu max=%zu", title_utf8.c_str(),
             shell::text_input_kind_name(kind), initial.size(), max_len);

  SceImeDialogParam param;
  fill_param(param, user_id_, kind, static_cast<std::uint32_t>(max_len), buffer_.data(),
             title_.empty() ? nullptr : title_.c_str());
  int rc = sceImeDialogInit(&param, nullptr);
  if (rc != 0) {
    log_printf("ime: error 0x%08x (sceImeDialogInit), retrying with plain parameters",
               static_cast<unsigned>(rc));
    make_plain(param);
    rc = sceImeDialogInit(&param, nullptr);
  }
  if (rc != 0) {
    log_printf("ime: error 0x%08x (sceImeDialogInit)", static_cast<unsigned>(rc));
    return rc;
  }
  open_ = true;
  sdl_return_ = false;
  last_status_ = -1;
  start_polls_ = 0;
  seen_running_ = false;
  password_ = kind == shell::TextInputKind::Password;
  log_line("ime: init ok");
  return 0;
}

Ps5SystemIme::Status Ps5SystemIme::poll() {
  Status out;
  if (!open_) {
    return out;
  }
  const int status = sceImeDialogGetStatus();
  if (status != last_status_) {
    last_status_ = status;
    log_printf("ime: status %s (%d)", status_name(status), status);
  }

  auto finish_with_text = [this, &out]() {
    out.state = State::Done;
    out.text = core::utf16_to_utf8(std::u16string_view(buffer_.data(), buffer_.size()));
    if (password_) {
      log_printf("ime: result <%zu bytes, hidden>", out.text.size());
    } else {
      log_printf("ime: result \"%s\"", out.text.c_str());
    }
  };

  switch (status) {
    case SCE_IME_DIALOG_STATUS_RUNNING:
      seen_running_ = true;
      out.state = State::Running;
      return out;

    case SCE_IME_DIALOG_STATUS_FINISHED: {
      SceImeDialogResult result{};
      const int rc = sceImeDialogGetResult(&result);
      sceImeDialogTerm();
      open_ = false;
      if (rc != 0) {
        log_printf("ime: error 0x%08x (sceImeDialogGetResult)", static_cast<unsigned>(rc));
        out.state = State::Error;
        out.error = rc;
        return out;
      }
      switch (result.endstatus) {
        case SCE_IME_DIALOG_END_STATUS_OK: finish_with_text(); break;
        case SCE_IME_DIALOG_END_STATUS_USER_CANCELED:
          log_line("ime: status cancelled");
          out.state = State::Cancelled;
          break;
        case SCE_IME_DIALOG_END_STATUS_ABORTED:
          log_line("ime: status cancelled (aborted by the system)");
          out.state = State::Cancelled;
          break;
        default:
          log_printf("ime: error unknown end status %d", static_cast<int>(result.endstatus));
          out.state = State::Error;
          out.error = result.endstatus;
          break;
      }
      return out;
    }

    case SCE_IME_DIALOG_STATUS_NONE:
      if (!seen_running_ && !sdl_return_ && ++start_polls_ < kStartPolls) {
        out.state = State::Running;  // not up yet
        return out;
      }
      open_ = false;
      if (!seen_running_ && !sdl_return_) {
        log_line("ime: error dialog never reported running");
        out.state = State::Error;
        out.error = -1;
      } else if (sdl_return_) {
        log_line("ime: dialog ended by the SDL pump with OK (Return seen)");
        finish_with_text();
      } else {
        log_line("ime: dialog ended by the SDL pump without Return, treating as cancelled");
        out.state = State::Cancelled;
      }
      return out;

    default:
      log_printf("ime: error 0x%08x (sceImeDialogGetStatus)", static_cast<unsigned>(status));
      sceImeDialogTerm();
      open_ = false;
      out.state = State::Error;
      out.error = status;
      return out;
  }
}

void Ps5SystemIme::term() {
  if (!open_) {
    return;
  }
  const int rc = sceImeDialogTerm();
  open_ = false;
  log_printf("ime: term (0x%08x)", static_cast<unsigned>(rc));
}

// --- Ps5ImeTextInput -------------------------------------------------------

bool Ps5ImeTextInput::request_text(const shell::TextInputRequest& request, Completion done) {
  if (done_) {
    return false;
  }
  const std::int32_t rc =
      ime_.open(request.title, request.initial, request.max_length, request.kind);
  if (rc != 0) {
    log_printf("ime: error 0x%08x, falling back to the built-in OSK",
               static_cast<unsigned>(rc));
    return false;
  }
  done_ = std::move(done);
  return true;
}

void Ps5ImeTextInput::poll() {
  if (!done_) {
    return;
  }
  Ps5SystemIme::Status status = ime_.poll();
  shell::TextInputResult result;
  switch (status.state) {
    case Ps5SystemIme::State::Running: return;
    case Ps5SystemIme::State::Done:
      result.outcome = shell::TextInputResult::Outcome::Done;
      result.text = std::move(status.text);
      break;
    case Ps5SystemIme::State::Idle:
    case Ps5SystemIme::State::Cancelled:
      result.outcome = shell::TextInputResult::Outcome::Cancelled;
      break;
    case Ps5SystemIme::State::Error:
      log_printf("ime: error 0x%08x, falling back to the built-in OSK",
                 static_cast<unsigned>(status.error));
      result.outcome = shell::TextInputResult::Outcome::Error;
      result.error = status.error;
      break;
  }
  Completion done = std::move(done_);
  done_ = nullptr;
  done(result);
}

bool use_system_ime(const std::string& config_value) {
  const char* env = std::getenv("PROSPEROWEB_OSK");
  const std::string value = env && *env ? std::string(env) : config_value;
  return value != "builtin";
}

}  // namespace prosperoweb::frontend
