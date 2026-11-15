#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "i18n/locale.hpp"
#include "i18n/tr.hpp"

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void with_lang(const char* lang, const std::function<void()>& fn) {
  unsetenv("LC_ALL");
  unsetenv("LC_MESSAGES");
  if (lang != nullptr) {
    setenv("LANG", lang, 1);
  }
  fn();
}

}  // namespace

int main() {
  try {
    tuide::i18n::set_locale(tuide::i18n::UiLocale::kEs);
    check(tuide::i18n::tr("welcome.tagline") == "depura y edita desde la terminal",
          "Spanish welcome tagline");
    check(tuide::i18n::tr("common.yes") == " Sí ", "Spanish yes");

    tuide::i18n::set_locale(tuide::i18n::UiLocale::kEn);
    check(tuide::i18n::tr("welcome.tagline") == "debug and edit from the terminal",
          "English welcome tagline");
    check(tuide::i18n::tr("common.yes") == " Yes ", "English yes");

    tuide::i18n::set_locale(tuide::i18n::UiLocale::kAuto);
    with_lang("en_US.UTF-8", [] {
      check(tuide::i18n::detect_system_locale() == tuide::i18n::UiLocale::kEn,
            "detect en from LANG");
    });
    with_lang("es_ES.UTF-8", [] {
      check(tuide::i18n::detect_system_locale() == tuide::i18n::UiLocale::kEs,
            "detect es from LANG");
    });

    // Anything that is not Spanish falls back to English, and LC_ALL /
    // LC_MESSAGES take precedence over LANG.
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    unsetenv("LANG");
    check(tuide::i18n::detect_system_locale() == tuide::i18n::UiLocale::kEn,
          "unset environment defaults to en");
    for (const char* lang : {"C", "C.UTF-8", "POSIX", "de_DE.UTF-8", "fr_FR.UTF-8"}) {
      setenv("LANG", lang, 1);
      check(tuide::i18n::detect_system_locale() == tuide::i18n::UiLocale::kEn,
            "non-Spanish LANG defaults to en");
    }
    setenv("LANG", "en_US.UTF-8", 1);
    setenv("LC_MESSAGES", "es_MX.UTF-8", 1);
    check(tuide::i18n::detect_system_locale() == tuide::i18n::UiLocale::kEs,
          "LC_MESSAGES overrides LANG");
    setenv("LC_ALL", "C.UTF-8", 1);
    check(tuide::i18n::detect_system_locale() == tuide::i18n::UiLocale::kEn,
          "LC_ALL overrides LC_MESSAGES");
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");

    tuide::i18n::set_locale(tuide::i18n::UiLocale::kEn);
    check(tuide::i18n::tr("nonexistent.key") == "[nonexistent.key]", "missing key marker");

    tuide::i18n::set_locale(tuide::i18n::UiLocale::kEs);
    check(tuide::i18n::tr_fmt("settings.locale.mode_label", {"Auto"}) == "Idioma (Auto)",
          "tr_fmt placeholder");

    std::cout << "i18n_test: all checks passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "i18n_test failed: " << ex.what() << '\n';
    return 1;
  }
}
