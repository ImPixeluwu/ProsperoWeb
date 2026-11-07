// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <set>
#include <string>

#include "prosperoweb/core/i18n.h"

using namespace prosperoweb::core;

namespace {

constexpr auto kCount = static_cast<std::size_t>(Text::Count);

Text text_at(std::size_t i) { return static_cast<Text>(i); }

}  // namespace

TEST_SUITE("i18n") {
  TEST_CASE("language codes") {
    CHECK(language_from_code("es") == Language::Es);
    CHECK(language_from_code("es-ES") == Language::Es);
    CHECK(language_from_code("es_MX.UTF-8") == Language::Es);
    CHECK(language_from_code(" ES ") == Language::Es);
    CHECK(language_from_code("ru") == Language::Ru);
    CHECK(language_from_code("ru_RU.UTF-8") == Language::Ru);
    CHECK(language_from_code("en") == Language::En);
    CHECK(language_from_code("de-DE") == Language::En);
    CHECK(language_from_code("") == Language::En);
    CHECK(language_code(Language::Es) == "es");
    CHECK(language_code(Language::Ru) == "ru");
    CHECK(language_code(Language::En) == "en");
  }

  TEST_CASE("every text has a unique id and English text") {
    std::set<std::string> ids;
    for (std::size_t i = 0; i < kCount; ++i) {
      CAPTURE(i);
      const auto id = std::string(text_id(text_at(i)));
      CHECK_FALSE(id.empty());
      CHECK(ids.insert(id).second);
      CHECK(has_translation(Language::En, text_at(i)));
    }
  }

  TEST_CASE("Spanish is complete") {
    for (std::size_t i = 0; i < kCount; ++i) {
      CAPTURE(text_id(text_at(i)));
      CHECK(has_translation(Language::Es, text_at(i)));
    }
  }

  TEST_CASE("placeholders survive translation") {
    // A translation must keep every {n} of the English text.
    for (std::size_t i = 0; i < kCount; ++i) {
      const auto en = tr(Language::En, text_at(i));
      for (const auto lang : {Language::Ru, Language::Es}) {
        const auto t = tr(lang, text_at(i));
        for (char n = '0'; n <= '9'; ++n) {
          const std::string ph = std::string("{") + n + "}";
          CAPTURE(text_id(text_at(i)));
          CAPTURE(ph);
          CHECK((en.find(ph) == std::string_view::npos) == (t.find(ph) == std::string_view::npos));
        }
      }
    }
  }

  TEST_CASE("missing translations fall back to English") {
    CHECK_FALSE(has_translation(Language::Ru, Text::MenuBookmarks));
    CHECK(tr(Language::Ru, Text::MenuBookmarks) == "Bookmarks");
    CHECK(tr(Language::Es, Text::MenuBookmarks) == "Marcadores");
    CHECK(tr(Language::Ru, Text::InputAddress) == "Адрес или поиск");
    CHECK(text_id(Text::MenuBookmarks) == "menu.bookmarks");
  }

  TEST_CASE("format_text") {
    CHECK(format_text("hace {0} min", {"5"}) == "hace 5 min");
    CHECK(format_text("{1} and {0}", {"a", "b"}) == "b and a");
    CHECK(format_text("{2} stays", {"a"}) == "{2} stays");
    CHECK(format_text("{x} and {", {"a"}) == "{x} and {");
    CHECK(format_text("", {}) == "");
  }
}
