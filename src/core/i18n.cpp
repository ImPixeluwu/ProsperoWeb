// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/i18n.h"

#include <array>
#include <cctype>

#include "text_utils.h"

namespace prosperoweb::core {

namespace {

struct Row {
  Text text;
  std::string_view id;  // key for the HTML chrome
  std::string_view en;  // complete; the fallback
  std::string_view ru;  // "" = not translated yet, English is shown
  std::string_view es;
};

// Keep in the order of the Text enum (tests/core/test_i18n.cpp checks it).
constexpr Row kRows[] = {
    {Text::On, "common.on", "On", "", "activado"},
    {Text::Off, "common.off", "Off", "", "desactivado"},

    {Text::TabLimitTitle, "notice.tab_limit.title", "Tab limit reached", "",
     "Límite de pestañas alcanzado"},
    {Text::TabLimitText, "notice.tab_limit.text",
     "Close a tab before opening a new one (at most {0} tabs).", "",
     "Cierra una pestaña antes de abrir otra (como máximo {0} pestañas)."},
    {Text::LoadFailedTitle, "notice.load_failed.title", "Could not load the page", "",
     "No se pudo cargar la página"},
    {Text::DrmTitle, "notice.drm.title", "Protected video is not supported",
     "Защищённое видео не поддерживается", "El video protegido no es compatible"},
    {Text::DrmText, "notice.drm.text",
     "This site requested protected (DRM) playback. This browser does not include a DRM module "
     "(Widevine, PlayReady or FairPlay), so protected videos cannot be played here. Free, "
     "non-protected videos on other sites are not affected. To watch this content, please use "
     "the service's official PS5 app or another supported device.",
     "Этот сайт запросил воспроизведение защищённого (DRM) видео. В этом браузере нет модуля DRM "
     "(Widevine, PlayReady или FairPlay), поэтому защищённое видео здесь воспроизвести нельзя. "
     "Обычное незащищённое видео на других сайтах работает как прежде. Чтобы посмотреть этот "
     "контент, воспользуйтесь официальным приложением сервиса для PS5 или другим поддерживаемым "
     "устройством.",
     "Este sitio pidió reproducir video protegido (DRM). Este navegador no incluye un módulo DRM "
     "(Widevine, PlayReady ni FairPlay), así que aquí no se pueden reproducir videos protegidos. "
     "Los videos gratuitos y sin protección de otros sitios no se ven afectados. Para ver este "
     "contenido, usa la app oficial del servicio para PS5 u otro dispositivo compatible."},

    {Text::InputAddress, "input.address", "Address or search", "Адрес или поиск",
     "Dirección o búsqueda"},
    {Text::InputText, "input.text", "Enter text", "Ввод текста", "Escribir texto"},

    {Text::MenuTitle, "menu.title", "Menu", "", "Menú"},
    {Text::MenuBookmarks, "menu.bookmarks", "Bookmarks", "", "Marcadores"},
    {Text::MenuHistory, "menu.history", "History", "", "Historial"},
    {Text::MenuAddBookmark, "menu.add_bookmark", "Add bookmark", "", "Añadir marcador"},
    {Text::MenuRemoveBookmark, "menu.remove_bookmark", "Remove bookmark", "", "Quitar marcador"},
    {Text::MenuNewTab, "menu.new_tab", "New tab", "", "Nueva pestaña"},
    {Text::MenuCloseTab, "menu.close_tab", "Close tab", "", "Cerrar pestaña"},
    {Text::MenuForward, "menu.forward", "Forward", "", "Adelante"},
    {Text::MenuReload, "menu.reload", "Reload", "", "Recargar"},
    {Text::MenuAdblock, "menu.adblock", "Ad blocker", "", "Bloqueador de anuncios"},
    {Text::MenuSponsorBlock, "menu.sponsorblock", "SponsorBlock", "", "SponsorBlock"},
    {Text::MenuNaturalScroll, "menu.natural_scroll", "Natural scrolling: {0}", "",
     "Desplazamiento natural: {0}"},
    {Text::MenuDiagnostics, "menu.diagnostics", "Diagnostics", "", "Diagnóstico"},
    {Text::MenuDpadLinks, "menu.dpad_links", "D-pad link navigation: {0}", "",
     "Navegación por enlaces con la cruceta: {0}"},
    {Text::MenuZoomIn, "menu.zoom_in", "Zoom in", "", "Acercar"},
    {Text::MenuZoomLevel, "menu.zoom_level", "Zoom: {0} (select to reset)", "",
     "Zoom: {0} (selecciona para restablecer)"},
    {Text::MenuZoomOut, "menu.zoom_out", "Zoom out", "", "Alejar"},
    {Text::MenuUserAgent, "menu.user_agent", "User agent: {0}", "", "User agent: {0}"},

    {Text::MenuUserscripts, "menu.userscripts", "User scripts", "", "Scripts de usuario"},
    {Text::MenuUserscriptsActive, "menu.userscripts_active", "User scripts ({0} active here)", "",
     "Scripts de usuario ({0} activos aquí)"},
    {Text::UserscriptsRemoveTitle, "menu.userscripts_remove", "Remove a user script", "",
     "Quitar un script de usuario"},
    {Text::UserscriptsNone, "userscripts.none", "No user scripts yet", "",
     "Aún no hay scripts de usuario"},
    {Text::UserscriptsHowTo, "userscripts.how_to", "Open a .user.js link to install one", "",
     "Abre un enlace .user.js para instalar uno"},
    {Text::UserscriptsItem, "userscripts.item", "{0}: {1}", "", "{0}: {1}"},
    {Text::UserscriptsRemoveMenu, "userscripts.remove_menu", "Remove a user script...", "",
     "Quitar un script de usuario..."},
    {Text::UserscriptsRemoveItem, "userscripts.remove_item", "Remove {0}", "", "Quitar {0}"},
    {Text::UserscriptInstallTitle, "userscript.install_title", "Install user script?", "",
     "¿Instalar el script de usuario?"},
    {Text::UserscriptUpdateTitle, "userscript.update_title", "Update user script?", "",
     "¿Actualizar el script de usuario?"},
    {Text::UserscriptRunsOn, "userscript.runs_on", "Runs on: {0}", "", "Se ejecuta en: {0}"},
    {Text::UserscriptTrustWarning, "userscript.trust_warning",
     "Only install scripts you trust: a user script can read and change every page it runs on.",
     "",
     "Instala solo scripts de confianza: un script de usuario puede leer y cambiar todas las "
     "páginas en las que se ejecuta."},
    {Text::UserscriptInstalledTitle, "userscript.installed_title", "User script installed", "",
     "Script de usuario instalado"},
    {Text::UserscriptInstalledText, "userscript.installed_text",
     "{0} runs from the next page load. Menu > User scripts turns it off or removes it.", "",
     "{0} se ejecutará desde la próxima carga de página. Menú > Scripts de usuario lo desactiva "
     "o lo quita."},
    {Text::MenuNoBookmarks, "menu.no_bookmarks", "No bookmarks yet", "",
     "Todavía no hay marcadores"},
    {Text::MenuHistoryEmpty, "menu.history_empty", "History is empty", "",
     "El historial está vacío"},
    {Text::MenuBack, "menu.back", "Back", "", "Atrás"},

    {Text::AdblockToggle, "adblock.toggle", "Ad blocker (EasyList/EasyPrivacy): {0}", "",
     "Bloqueador de anuncios (EasyList/EasyPrivacy): {0}"},
    {Text::AdblockCookies, "adblock.cookies", "Block cookie banners (EasyList-Cookie): {0}", "",
     "Bloquear avisos de cookies (EasyList-Cookie): {0}"},
    {Text::AdblockAutoUpdate, "adblock.auto_update", "Auto-update filter lists: {0}", "",
     "Actualizar listas de filtros automáticamente: {0}"},
    {Text::AdblockUpdateNow, "adblock.update_now", "Update filter lists now", "",
     "Actualizar listas de filtros ahora"},
    {Text::AdblockUpdated, "adblock.updated", "Filter lists updated: {0}", "",
     "Listas de filtros actualizadas: {0}"},
    {Text::AdblockUpdating, "adblock.updating", "updating...", "", "actualizando..."},
    {Text::SponsorBlockToggle, "sponsorblock.toggle", "SponsorBlock: {0}", "",
     "SponsorBlock: {0}"},

    {Text::MenuPopups, "menu.popups", "Pop-up blocker", "", "Bloqueador de ventanas emergentes"},
    {Text::PopupToggle, "popup.toggle", "Block pop-ups: {0}", "", "Bloquear ventanas emergentes: {0}"},
    {Text::PopupBlockedCount, "popup.blocked_count", "Blocked since start: {0}", "",
     "Bloqueadas desde el inicio: {0}"},
    {Text::PopupNoSites, "popup.no_sites", "No sites allowed to open pop-ups", "",
     "Ningún sitio puede abrir ventanas emergentes"},
    {Text::PopupAllowedSite, "popup.allowed_site", "Allowed: {0} (select to remove)", "",
     "Permitido: {0} (selecciona para quitarlo)"},
    {Text::PopupBlockedTitle, "popup.blocked_title", "Pop-up blocked", "",
     "Ventana emergente bloqueada"},
    {Text::PopupBlockedText, "popup.blocked_text", "{0} tried to open a new window:\n{1}", "",
     "{0} intentó abrir una ventana nueva:\n{1}"},
    {Text::PopupBlockedBlank, "popup.blocked_blank", "{0} tried to open a blank window.", "",
     "{0} intentó abrir una ventana en blanco."},
    {Text::PopupThisPage, "popup.this_page", "This page", "", "Esta página"},
    {Text::PopupOpen, "popup.open", "Open", "", "Abrir"},
    {Text::PopupDismiss, "popup.dismiss", "Dismiss", "", "Descartar"},
    {Text::PopupAlwaysAllow, "popup.always_allow", "Always allow", "", "Permitir siempre"},
    {Text::AgoNever, "ago.never", "never", "", "nunca"},
    {Text::AgoJustNow, "ago.just_now", "just now", "", "ahora mismo"},
    {Text::AgoMinutes, "ago.minutes", "{0} min ago", "", "hace {0} min"},
    {Text::AgoHours, "ago.hours", "{0} h ago", "", "hace {0} h"},
    {Text::AgoDays, "ago.days", "{0} d ago", "", "hace {0} d"},

    {Text::KeyShift, "key.shift", "Shift", "", "Mayús"},
    {Text::KeySpace, "key.space", "Space", "", "Espacio"},
    {Text::KeyBackspace, "key.backspace", "Backspace", "", "Borrar"},
    {Text::KeyEnter, "key.enter", "Enter", "", "Intro"},
    {Text::KeyClear, "key.clear", "Clear", "", "Limpiar"},
    {Text::KeyGo, "key.go", "Go", "", "Ir"},

    {Text::ChromeSearchPlaceholder, "chrome.search_placeholder", "Search or enter address", "",
     "Busca o escribe una dirección"},
    {Text::ChromeBackHint, "chrome.back_hint", "Back (Circle)", "", "Atrás (Círculo)"},
    {Text::ChromeForward, "chrome.forward", "Forward", "", "Adelante"},
    {Text::ChromeReloadHint, "chrome.reload_hint", "Reload (Create)", "", "Recargar (Create)"},
    {Text::ChromeStop, "chrome.stop", "Stop", "", "Detener"},
    {Text::ChromeMenuHint, "chrome.menu_hint", "Menu (Options)", "", "Menú (Options)"},
    {Text::ChromeNewTab, "chrome.new_tab", "New tab", "", "Nueva pestaña"},
    {Text::ChromeCloseTab, "chrome.close_tab", "Close tab", "", "Cerrar pestaña"},
    {Text::ChromeSelect, "chrome.select", "Select", "", "Seleccionar"},
    {Text::ChromeOk, "chrome.ok", "OK", "", "Aceptar"},
    {Text::ChromeCancel, "chrome.cancel", "Cancel", "", "Cancelar"},
    {Text::OskTargetAddress, "osk.target_address", "Address", "", "Dirección"},
    {Text::OskTargetText, "osk.target_text", "Text field", "", "Campo de texto"},
    {Text::OskHintKey, "osk.hint_key", "Key", "", "Tecla"},
    {Text::OskHintDelete, "osk.hint_delete", "Delete (hold to repeat)", "",
     "Borrar (mantén para repetir)"},
    {Text::OskHintCaret, "osk.hint_caret", "Caret", "", "Cursor"},
    {Text::OskHintShift, "osk.hint_shift", "Shift", "", "Mayús"},
    {Text::OskHintLayout, "osk.hint_layout", "Layout", "", "Distribución"},
    {Text::OskHintDone, "osk.hint_done", "Done", "", "Listo"},

    {Text::SplashLoading, "splash.loading", "Loading…", "", "Cargando…"},
    {Text::LoaderNoConnect1, "loader.no_connect.1",
     "ProsperoWeb started its loader but could not connect.", "",
     "ProsperoWeb inició su cargador, pero no pudo conectarse."},
    {Text::LoaderNoConnect2, "loader.no_connect.2",
     "Waiting for the spawn broker on 127.0.0.1:17771 to answer.", "",
     "Esperando a que responda el spawn broker en 127.0.0.1:17771."},
    {Text::LoaderNoConnect3, "loader.no_connect.3",
     "Still trying — it should start on its own in a moment.", "",
     "Sigue intentándolo: debería arrancar solo en un momento."},
    {Text::LoaderMissing1, "loader.missing.1", "ProsperoWeb could not reach an ELF loader.", "",
     "ProsperoWeb no encontró ningún cargador ELF."},
    {Text::LoaderMissing2, "loader.missing.2",
     "It needs a loader on 127.0.0.1:9021 to start its web engine.", "",
     "Necesita un cargador en 127.0.0.1:9021 para iniciar su motor web."},
    {Text::LoaderMissing3, "loader.missing.3",
     "Start etaHEN or elfldr — it will continue on its own, no relaunch.", "",
     "Inicia etaHEN o elfldr: continuará solo, sin volver a abrir la app."},
};

static_assert(std::size(kRows) == static_cast<std::size_t>(Text::Count),
              "every Text needs a row in kRows");

const Row& row(Text text) {
  const auto i = static_cast<std::size_t>(text);
  return kRows[i < std::size(kRows) ? i : 0];
}

std::string_view own_text(Language lang, const Row& r) {
  switch (lang) {
    case Language::Ru: return r.ru;
    case Language::Es: return r.es;
    case Language::En: break;
  }
  return r.en;
}

}  // namespace

Language language_from_code(std::string_view code) {
  const std::string c = detail::to_lower_ascii(detail::trim(code));
  if (c.starts_with("es")) {
    return Language::Es;
  }
  if (c.starts_with("ru")) {
    return Language::Ru;
  }
  return Language::En;
}

std::string_view language_code(Language lang) {
  switch (lang) {
    case Language::Ru: return "ru";
    case Language::Es: return "es";
    case Language::En: break;
  }
  return "en";
}

std::string_view tr(Language lang, Text text) {
  const Row& r = row(text);
  const std::string_view own = own_text(lang, r);
  return own.empty() ? r.en : own;
}

std::string_view text_id(Text text) { return row(text).id; }

bool has_translation(Language lang, Text text) { return !own_text(lang, row(text)).empty(); }

std::string format_text(std::string_view pattern, std::initializer_list<std::string_view> args) {
  std::string out;
  out.reserve(pattern.size());
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '{' && i + 2 < pattern.size() &&
        std::isdigit(static_cast<unsigned char>(pattern[i + 1])) && pattern[i + 2] == '}') {
      const auto n = static_cast<std::size_t>(pattern[i + 1] - '0');
      if (n < args.size()) {
        out += *(args.begin() + n);
        i += 2;
        continue;
      }
    }
    out += pattern[i];
  }
  return out;
}

}  // namespace prosperoweb::core
