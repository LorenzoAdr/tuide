#include "i18n/locale.hpp"

#include <cstdlib>
#include <cstring>

namespace tuide::i18n {

namespace {

UiLocale g_locale_setting = UiLocale::kAuto;
UiLocale g_effective_locale = UiLocale::kEn;

bool starts_with_ci(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const char a = text[i];
    const char b = prefix[i];
    if (a >= 'A' && a <= 'Z') {
      if (a + ('a' - 'A') != b && a != b) {
        return false;
      }
    } else if (a != b) {
      return false;
    }
  }
  return true;
}

}  // namespace

// POSIX precedence for message language: LC_ALL, then LC_MESSAGES, then LANG.
// Spanish only when the system asks for it; English for everything else,
// including C/POSIX and unset environments.
UiLocale detect_system_locale() {
  for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
      continue;
    }
    return starts_with_ci(value, "es") ? UiLocale::kEs : UiLocale::kEn;
  }
  return UiLocale::kEn;
}

UiLocale resolve_locale(UiLocale setting) {
  if (setting == UiLocale::kAuto) {
    return detect_system_locale();
  }
  return setting;
}

void set_locale(UiLocale locale) {
  g_locale_setting = locale;
  g_effective_locale = resolve_locale(locale);
}

UiLocale current_locale_setting() { return g_locale_setting; }

UiLocale effective_locale() { return g_effective_locale; }

const char* locale_tag(UiLocale locale) {
  switch (locale) {
    case UiLocale::kEs:
      return "es";
    case UiLocale::kEn:
      return "en";
    case UiLocale::kAuto:
    default:
      return "auto";
  }
}

UiLocale parse_locale(std::string_view tag) {
  if (tag == "en") {
    return UiLocale::kEn;
  }
  if (tag == "es") {
    return UiLocale::kEs;
  }
  return UiLocale::kAuto;
}

}  // namespace tuide::i18n
