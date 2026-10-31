// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/shell/text_input.h"

namespace prosperoweb::shell {

const char* text_input_kind_name(TextInputKind kind) {
  switch (kind) {
    case TextInputKind::Text: return "text";
    case TextInputKind::Url: return "url";
    case TextInputKind::Search: return "search";
    case TextInputKind::Email: return "email";
    case TextInputKind::Number: return "number";
    case TextInputKind::Password: return "password";
  }
  return "text";
}

TextInputKind text_input_kind_from_type(const std::string& input_type) {
  if (input_type == "url") return TextInputKind::Url;
  if (input_type == "search") return TextInputKind::Search;
  if (input_type == "email") return TextInputKind::Email;
  if (input_type == "number") return TextInputKind::Number;
  if (input_type == "password") return TextInputKind::Password;
  return TextInputKind::Text;
}

}  // namespace prosperoweb::shell
