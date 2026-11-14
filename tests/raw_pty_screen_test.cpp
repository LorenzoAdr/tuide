#include "terminal/raw_pty_screen.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void test_basic_ansi_colors() {
  tuide::RawPtyScreen screen(4, 40);
  const std::string input = "\033[32mgreen\033[0m plain";
  screen.feed(input.data(), input.size());
  const auto rows = screen.styled_rows();
  expect(!rows.empty(), "basic rows");
  const auto& row = rows.back();
  expect(row.size() >= 2, "basic spans");
  expect(row[0].text == "green", "basic green text");
  expect(row[0].fg == ftxui::Color::RGB(80, 200, 80), "basic green fg");
  expect(row[1].text.find("plain") != std::string::npos, "basic plain text");
}

void test_256_color_prompt() {
  // Mirrors a typical bash PS1 fragment: \033[38;5;166muser\033[0m \033[38;5;2m/path
  tuide::RawPtyScreen screen(4, 80);
  const std::string input =
      "[\033[38;5;166muser\033[0m \033[38;5;2m/home/proj\033[0m] ";
  screen.feed(input.data(), input.size());
  const auto rows = screen.styled_rows();
  expect(!rows.empty(), "256 rows");
  const auto& row = rows.back();

  bool found_user = false;
  bool found_path = false;
  for (const auto& span : row) {
    if (span.text == "user") {
      found_user = true;
      expect(span.fg == ftxui::Color::RGB(215, 95, 0), "256 user orange");
    }
    if (span.text == "/home/proj") {
      found_path = true;
      expect(span.fg == ftxui::Color::RGB(80, 200, 80), "256 path green");
    }
  }
  expect(found_user, "256 found user span");
  expect(found_path, "256 found path span");
}

void test_truecolor() {
  tuide::RawPtyScreen screen(4, 40);
  const std::string input = "\033[38;2;10;20;30mrgb\033[0m";
  screen.feed(input.data(), input.size());
  const auto rows = screen.styled_rows();
  expect(!rows.empty(), "truecolor rows");
  const auto& row = rows.back();
  expect(!row.empty(), "truecolor spans");
  expect(row[0].text == "rgb", "truecolor text");
  expect(row[0].fg == ftxui::Color::RGB(10, 20, 30), "truecolor fg");
}

void test_256_background() {
  tuide::RawPtyScreen screen(4, 40);
  const std::string input = "\033[48;5;196mx\033[0m";
  screen.feed(input.data(), input.size());
  const auto rows = screen.styled_rows();
  expect(!rows.empty() && !rows.back().empty(), "bg rows");
  expect(rows.back()[0].bg == ftxui::Color::RGB(255, 0, 0), "256 bg red");
  expect(!rows.back()[0].bg_default, "256 bg not default");
}

void test_default_background_flag() {
  tuide::RawPtyScreen screen(4, 40);
  const std::string input = "\033[48;5;196mx\033[49my\033[0mz";
  screen.feed(input.data(), input.size());
  const auto rows = screen.styled_rows();
  expect(!rows.empty() && rows.back().size() >= 2, "default bg spans");
  expect(rows.back()[0].text == "x" && !rows.back()[0].bg_default, "explicit red bg");
  // SGR 49 and SGR 0 both restore default bg with the same fg, so "y"/"z" merge.
  expect(rows.back()[1].text.find('y') != std::string::npos && rows.back()[1].bg_default,
         "sgr 49 default bg");
  expect(rows.back()[1].text.find('z') != std::string::npos && rows.back()[1].bg_default,
         "sgr 0 default bg");
}

bool same_rows(const std::vector<tuide::TerminalStyledRow>& a,
               const std::vector<tuide::TerminalStyledRow>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].size() != b[i].size()) {
      return false;
    }
    for (std::size_t j = 0; j < a[i].size(); ++j) {
      if (a[i][j].text != b[i][j].text || a[i][j].fg != b[i][j].fg ||
          a[i][j].bg != b[i][j].bg || a[i][j].bg_default != b[i][j].bg_default) {
        return false;
      }
    }
  }
  return true;
}

void feed(tuide::RawPtyScreen* screen, const std::string& s) { screen->feed(s.data(), s.size()); }

// La UI no copia ya el historial entero en cada tecla: lo que sincroniza de
// forma incremental tiene que acabar igual que la copia completa.
void test_incremental_sync_matches_full_copy() {
  tuide::RawPtyScreen screen(10, 40);
  std::vector<tuide::TerminalStyledRow> mirror;
  tuide::TerminalRowsSyncCursor cursor;
  auto check = [&](const char* what) {
    screen.sync_styled_rows(&mirror, &cursor);
    expect(same_rows(mirror, screen.styled_rows()), what);
  };
  check("sync empty screen");
  feed(&screen, "\033[32m$ \033[0m");
  check("sync prompt");
  for (char ch : std::string("echo hola")) {
    feed(&screen, std::string(1, ch));
    check("sync per keystroke");
  }
  feed(&screen, "\b\033[K");
  check("sync backspace");
  feed(&screen, "\r\nhola\r\n$ ");
  check("sync after newline");

  // Llenar más allá del tope del scrollback para forzar recortes por delante,
  // sincronizando a ritmos distintos (cada línea y a ráfagas).
  for (int i = 0; i < 10600; ++i) {
    feed(&screen, "line " + std::to_string(i) + "\r\n");
    if (i % 97 == 0) {
      check("sync while trimming");
    }
  }
  check("sync after trim burst");
  expect(mirror.size() <= 10000 + 512 + 1, "scrollback stays bounded");
  expect(mirror.front().front().text.rfind("line ", 0) == 0, "oldest kept line");

  screen.resize(12, 20);
  check("sync after resize");
  feed(&screen, "abc\r\n");
  check("sync after resize + output");

  // Un espejo tocado por fuera (la UI lo vacía al reiniciar) se recompone.
  mirror.clear();
  check("sync after external clear");

  screen.reset(10, 40);
  check("sync after reset");
  feed(&screen, "x\r\ny");
  check("sync after reset + output");
}

// Un cursor de otra pantalla no debe hacer creer que el espejo ya está al día.
void test_sync_cursor_is_per_screen() {
  tuide::RawPtyScreen a(5, 20);
  tuide::RawPtyScreen b(5, 20);
  feed(&a, "uno\r\ndos\r\n");
  feed(&b, "tres\r\ncuatro\r\n");
  std::vector<tuide::TerminalStyledRow> mirror;
  tuide::TerminalRowsSyncCursor cursor;
  a.sync_styled_rows(&mirror, &cursor);
  b.sync_styled_rows(&mirror, &cursor);
  expect(same_rows(mirror, b.styled_rows()), "cursor from another screen forces full copy");
}

}  // namespace

int main() {
  test_basic_ansi_colors();
  test_256_color_prompt();
  test_truecolor();
  test_256_background();
  test_default_background_flag();
  test_incremental_sync_matches_full_copy();
  test_sync_cursor_is_per_screen();
  std::cout << "raw_pty_screen_test: ok\n";
  return 0;
}
