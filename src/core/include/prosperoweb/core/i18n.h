// SPDX-License-Identifier: GPL-3.0-or-later
//
// User-interface strings in every supported language. One table (i18n.cpp)
// feeds the shell (menus, notices, text-entry titles), the HTML chrome (sent
// once as "strings" in the chrome state), the on-screen keyboard's special
// keys and the PS5 splash screens. English is complete and is the fallback:
// an empty translation shows the English text.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace prosperoweb::core {

enum class Language : std::uint8_t { En, Ru, Es };

// Parses a config.json / --lang / $LANG value: "es", "es-ES", "es_MX.UTF-8"
// give Es, "ru..." gives Ru, anything else (or empty) English.
Language language_from_code(std::string_view code);
// "en", "ru" or "es".
std::string_view language_code(Language lang);

// Every translatable string. The order matches the table in i18n.cpp (a unit
// test checks it); text_id() is the stable key the HTML chrome uses.
enum class Text : std::uint16_t {
  // Words reused in labels
  On,
  Off,
  // Notices
  TabLimitTitle,
  TabLimitText,  // {0} = maximum number of tabs
  LoadFailedTitle,
  DrmTitle,
  DrmText,
  // System keyboard dialog titles
  InputAddress,
  InputText,
  // Menu
  MenuTitle,
  MenuBookmarks,
  MenuHistory,
  MenuAddBookmark,
  MenuRemoveBookmark,
  MenuNewTab,
  MenuCloseTab,
  MenuForward,
  MenuReload,
  MenuAdblock,
  MenuSponsorBlock,
  MenuNaturalScroll,  // {0} = On / Off
  MenuDiagnostics,
  MenuNoBookmarks,
  MenuHistoryEmpty,
  MenuBack,
  // Ad blocker and SponsorBlock sections
  AdblockToggle,       // {0} = On / Off
  AdblockCookies,      // {0} = On / Off
  AdblockAutoUpdate,   // {0} = On / Off
  AdblockUpdateNow,
  AdblockUpdated,      // {0} = when (see the Ago* texts)
  AdblockUpdating,
  SponsorBlockToggle,  // {0} = On / Off
  AgoNever,
  AgoJustNow,
  AgoMinutes,  // {0} = minutes
  AgoHours,    // {0} = hours
  AgoDays,     // {0} = days
  // On-screen keyboard special keys
  KeyShift,
  KeySpace,
  KeyBackspace,
  KeyEnter,
  KeyClear,
  KeyGo,
  // HTML chrome
  ChromeSearchPlaceholder,
  ChromeBackHint,
  ChromeForward,
  ChromeReloadHint,
  ChromeStop,
  ChromeMenuHint,
  ChromeNewTab,
  ChromeCloseTab,
  ChromeSelect,
  ChromeOk,
  ChromeCancel,
  OskTargetAddress,
  OskTargetText,
  OskHintKey,
  OskHintDelete,
  OskHintCaret,
  OskHintShift,
  OskHintLayout,
  OskHintDone,
  // PS5 splash and loader screens
  SplashLoading,
  LoaderNoConnect1,
  LoaderNoConnect2,
  LoaderNoConnect3,
  LoaderMissing1,
  LoaderMissing2,
  LoaderMissing3,
  Count
};

// The text in lang, or the English text when lang has no translation.
std::string_view tr(Language lang, Text text);
// The chrome's key for text, e.g. "menu.bookmarks".
std::string_view text_id(Text text);
// True when lang has its own (non-fallback) text.
bool has_translation(Language lang, Text text);

// Replaces {0}, {1}, ... in pattern with args; other braces are kept.
std::string format_text(std::string_view pattern, std::initializer_list<std::string_view> args);

}  // namespace prosperoweb::core
